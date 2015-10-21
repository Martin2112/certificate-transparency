#include "server/handler.h"

#include <algorithm>
#include <event2/buffer.h>
#include <event2/http.h>
#include <event2/keyvalq_struct.h>
#include <functional>
#include <gflags/gflags.h>
#include <glog/logging.h>
#include <map>
#include <memory>
#include <stdlib.h>
#include <utility>
#include <vector>

#include "log/cert.h"
#include "log/cert_checker.h"
#include "log/cluster_state_controller.h"
#include "log/frontend.h"
#include "log/log_lookup.h"
#include "log/logged_certificate.h"
#include "monitoring/monitoring.h"
#include "monitoring/latency.h"
#include "server/json_output.h"
#include "server/proxy.h"
#include "util/json_wrapper.h"
#include "util/thread_pool.h"

namespace libevent = cert_trans::libevent;

using cert_trans::Cert;
using cert_trans::CertChain;
using cert_trans::CertChecker;
using cert_trans::Counter;
using cert_trans::HttpHandler;
using cert_trans::JsonOutput;
using cert_trans::Latency;
using cert_trans::LoggedCertificate;
using cert_trans::Proxy;
using cert_trans::ScopedLatency;
using ct::ShortMerkleAuditProof;
using ct::SignedCertificateTimestamp;
using ct::SignedTreeHead;
using std::bind;
using std::chrono::milliseconds;
using std::chrono::seconds;
using std::function;
using std::lock_guard;
using std::make_pair;
using std::make_shared;
using std::multimap;
using std::mutex;
using std::placeholders::_1;
using std::shared_ptr;
using std::string;
using std::to_string;
using std::unique_ptr;
using std::vector;

DEFINE_int32(max_leaf_entries_per_response, 1000,
             "maximum number of entries to put in the response of a "
             "get-entries request");
DEFINE_int32(staleness_check_delay_secs, 5,
             "number of seconds between node staleness checks");

namespace {


static Latency<milliseconds, string> http_server_request_latency_ms(
    "total_http_server_request_latency_ms", "path",
    "Total request latency in ms broken down by path");


multimap<string, string> ParseQuery(evhttp_request* req) {
  evkeyvalq keyval;
  multimap<string, string> retval;

  // We return an empty result in case of a parsing error.
  if (evhttp_parse_query_str(evhttp_uri_get_query(
                                 evhttp_request_get_evhttp_uri(req)),
                             &keyval) == 0) {
    for (evkeyval* i = keyval.tqh_first; i; i = i->next.tqe_next) {
      retval.insert(make_pair(i->key, i->value));
    }
  }

  return retval;
}


bool GetParam(const multimap<string, string>& query, const string& param,
              string* value) {
  CHECK_NOTNULL(value);

  multimap<string, string>::const_iterator it = query.find(param);
  if (it == query.end()) {
    return false;
  }

  const string possible_value(it->second);
  ++it;

  // Flag duplicate query parameters as invalid.
  const bool retval(it == query.end() || it->first != param);
  if (retval) {
    *value = possible_value;
  }

  return retval;
}


// Returns -1 on error, and on success too if the parameter contains
// -1 (so it's advised to only use it when expecting unsigned
// parameters).
int64_t GetIntParam(const multimap<string, string>& query,
                    const string& param) {
  int retval(-1);
  string value;
  if (GetParam(query, param, &value)) {
    errno = 0;
    const long num(strtol(value.c_str(), /*endptr*/ NULL, 10));
    // Detect strtol() errors or overflow/underflow when casting to
    // retval's type clips the value. We do the following by doing it,
    // and checking that they're still equal afterward (this will
    // still work if we change retval's type later on).
    retval = num;
    if (errno || static_cast<long>(retval) != num) {
      VLOG(1) << "over/underflow getting \"" << param << "\": " << retval
              << ", " << num << " (" << strerror(errno) << ")";
      retval = -1;
    }
  }

  return retval;
}


bool GetBoolParam(const multimap<string, string>& query, const string& param) {
  string value;
  if (GetParam(query, param, &value)) {
    return (value == "true");
  } else {
    return false;
  }
}


}  // namespace


HttpHandler::HttpHandler(
    JsonOutput* output, LogLookup<LoggedCertificate>* log_lookup,
    const ReadOnlyDatabase<LoggedCertificate>* db,
    const ClusterStateController<LoggedCertificate>* controller,
    const CertChecker* cert_checker, Frontend* frontend, Proxy* proxy,
    ThreadPool* pool, libevent::Base* event_base)
    : output_(CHECK_NOTNULL(output)),
      log_lookup_(CHECK_NOTNULL(log_lookup)),
      db_(CHECK_NOTNULL(db)),
      controller_(CHECK_NOTNULL(controller)),
      cert_checker_(cert_checker),
      frontend_(frontend),
      proxy_(CHECK_NOTNULL(proxy)),
      pool_(CHECK_NOTNULL(pool)),
      event_base_(CHECK_NOTNULL(event_base)),
      task_(pool_),
      node_is_stale_(controller_->NodeIsStale()) {
  event_base_->Delay(seconds(FLAGS_staleness_check_delay_secs),
                     task_.task()->AddChild(
                         bind(&HttpHandler::UpdateNodeStaleness, this)));
}


HttpHandler::~HttpHandler() {
  task_.task()->Return();
  task_.Wait();
}


void StatsHandlerInterceptor(const string& path,
                             const libevent::HttpServer::HandlerCallback& cb,
                             evhttp_request* req) {
  ScopedLatency total_http_server_request_latency(
      http_server_request_latency_ms.GetScopedLatency(path));

  cb(req);
}


void HttpHandler::ProxyInterceptor(
    const libevent::HttpServer::HandlerCallback& local_handler,
    evhttp_request* request) {
  VLOG(2) << "Running proxy interceptor...";
  // TODO(alcutter): We can be a bit smarter about when to proxy off
  // the request - being stale wrt to the current serving STH doesn't
  // automatically mean we're unable to answer this request.
  if (IsNodeStale()) {
    // Can't do this on the libevent thread since it can block on the lock in
    // ClusterStatusController::GetFreshNodes().
    pool_->Add(bind(&Proxy::ProxyRequest, proxy_, request));
  } else {
    local_handler(request);
  }
}


void HttpHandler::AddProxyWrappedHandler(
    libevent::HttpServer* server, const string& path,
    const libevent::HttpServer::HandlerCallback& local_handler) {
  const libevent::HttpServer::HandlerCallback stats_handler(
      bind(&StatsHandlerInterceptor, path, local_handler, _1));
  CHECK(server->AddHandler(path, bind(&HttpHandler::ProxyInterceptor, this,
                                      stats_handler, _1)));
}

bool HttpHandler::ExtractChain(JsonOutput* output, evhttp_request* req,
                               CertChain* chain) {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_POST) {
    output->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
    return false;
  }

  // TODO(pphaneuf): Should we check that Content-Type says
  // "application/json", as recommended by RFC4627?
  JsonObject json_body(evhttp_request_get_input_buffer(req));
  if (!json_body.Ok() || !json_body.IsType(json_type_object)) {
    output->SendError(req, HTTP_BADREQUEST, "Unable to parse provided JSON.");
    return false;
  }

  JsonArray json_chain(json_body, "chain");
  if (!json_chain.Ok()) {
    output->SendError(req, HTTP_BADREQUEST, "Unable to parse provided JSON.");
    return false;
  }

  VLOG(2) << "ExtractChain chain:\n" << json_chain.DebugString();

  for (int i = 0; i < json_chain.Length(); ++i) {
    JsonString json_cert(json_chain, i);
    if (!json_cert.Ok()) {
      output->SendError(req, HTTP_BADREQUEST,
                        "Unable to parse provided JSON.");
      return false;
    }

    unique_ptr<Cert> cert(new Cert);
    cert->LoadFromDerString(json_cert.FromBase64());
    if (!cert->IsLoaded()) {
      output->SendError(req, HTTP_BADREQUEST,
                        "Unable to parse provided chain.");
      return false;
    }

    chain->AddCert(cert.release());
  }

  return true;
}

void HttpHandler::AddChainReply(JsonOutput* output, evhttp_request* req,
                                const util::Status& add_status,
                                const SignedCertificateTimestamp& sct) {
  if (!add_status.ok() &&
      add_status.CanonicalCode() != util::error::ALREADY_EXISTS) {
    VLOG(1) << "error adding chain: " << add_status;
    const int response_code(add_status.CanonicalCode() ==
                                    util::error::RESOURCE_EXHAUSTED
                                ? HTTP_SERVUNAVAIL
                                : HTTP_BADREQUEST);
    return output->SendError(req, response_code, add_status.error_message());
  }

  JsonObject json_reply;
  json_reply.Add("sct_version", static_cast<int64_t>(0));
  json_reply.AddBase64("id", sct.id().key_id());
  json_reply.Add("timestamp", sct.timestamp());
  json_reply.Add("extensions", "");
  json_reply.Add("signature", sct.signature());

  output->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::GetEntries(evhttp_request* req) const {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    return output_->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
  }

  const multimap<string, string> query(ParseQuery(req));

  const int64_t start(GetIntParam(query, "start"));
  if (start < 0) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"start\" parameter.");
  }

  int64_t end(GetIntParam(query, "end"));
  if (end < start) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"end\" parameter.");
  }

  // Limit the number of entries returned in a single request.
  end = std::min(end, start + FLAGS_max_leaf_entries_per_response);

  // Sekrit parameter to indicate that SCTs should be included too.
  // This is non-standard, and is only used internally by other log nodes when
  // "following" nodes with more data.
  const bool include_scts(GetBoolParam(query, "include_scts"));

  BlockingGetEntries(req, start, end, include_scts);
}

void HttpHandler::GetRoots(evhttp_request* req) const {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    return output_->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
  }

  JsonArray roots;
  multimap<string, const Cert*>::const_iterator it;
  for (it = cert_checker_->GetTrustedCertificates().begin();
       it != cert_checker_->GetTrustedCertificates().end(); ++it) {
    string cert;
    if (it->second->DerEncoding(&cert) != util::Status::OK) {
      LOG(ERROR) << "Cert encoding failed";
      return output_->SendError(req, HTTP_INTERNAL, "Serialisation failed.");
    }
    roots.AddBase64(cert);
  }

  JsonObject json_reply;
  json_reply.Add("certificates", roots);

  output_->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::GetProof(evhttp_request* req) const {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    return output_->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
  }

  const multimap<string, string> query(ParseQuery(req));

  string b64_hash;
  if (!GetParam(query, "hash", &b64_hash)) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"hash\" parameter.");
  }

  const string hash(util::FromBase64(b64_hash.c_str()));
  if (hash.empty()) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Invalid \"hash\" parameter.");
  }

  const int64_t tree_size(GetIntParam(query, "tree_size"));
  if (tree_size < 0 ||
      static_cast<int64_t>(tree_size) > log_lookup_->GetSTH().tree_size()) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"tree_size\" parameter.");
  }

  ShortMerkleAuditProof proof;
  if (log_lookup_->AuditProof(hash, tree_size, &proof) !=
      LogLookup<LoggedCertificate>::OK) {
    return output_->SendError(req, HTTP_BADREQUEST, "Couldn't find hash.");
  }

  JsonArray json_audit;
  for (int i = 0; i < proof.path_node_size(); ++i) {
    json_audit.AddBase64(proof.path_node(i));
  }

  JsonObject json_reply;
  json_reply.Add("leaf_index", proof.leaf_index());
  json_reply.Add("audit_path", json_audit);

  output_->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::GetSTH(evhttp_request* req) const {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    return output_->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
  }

  const SignedTreeHead& sth(log_lookup_->GetSTH());

  VLOG(2) << "SignedTreeHead:\n" << sth.DebugString();

  JsonObject json_reply;
  json_reply.Add("tree_size", sth.tree_size());
  json_reply.Add("timestamp", sth.timestamp());
  json_reply.AddBase64("sha256_root_hash", sth.sha256_root_hash());
  json_reply.Add("tree_head_signature", sth.signature());

  VLOG(2) << "GetSTH:\n" << json_reply.DebugString();

  output_->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::GetConsistency(evhttp_request* req) const {
  if (evhttp_request_get_command(req) != EVHTTP_REQ_GET) {
    return output_->SendError(req, HTTP_BADMETHOD, "Method not allowed.");
  }

  const multimap<string, string> query(ParseQuery(req));

  const int64_t first(GetIntParam(query, "first"));
  if (first < 0) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"first\" parameter.");
  }

  const int64_t second(GetIntParam(query, "second"));
  if (second < first) {
    return output_->SendError(req, HTTP_BADREQUEST,
                              "Missing or invalid \"second\" parameter.");
  }

  const vector<string> consistency(
      log_lookup_->ConsistencyProof(first, second));
  JsonArray json_cons;
  for (vector<string>::const_iterator it = consistency.begin();
       it != consistency.end(); ++it) {
    json_cons.AddBase64(*it);
  }

  JsonObject json_reply;
  json_reply.Add("consistency", json_cons);

  output_->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::AddChain(evhttp_request* req) {
  const shared_ptr<CertChain> chain(make_shared<CertChain>());
  if (!ExtractChain(output_, req, chain.get())) {
    return;
  }

  pool_->Add(bind(&HttpHandler::BlockingAddChain, this, req, chain));
}


void HttpHandler::BlockingGetEntries(evhttp_request* req, int64_t start,
                                     int64_t end, bool include_scts) const {
  JsonArray json_entries;
  auto it(db_->ScanEntries(start));
  for (int64_t i = start; i <= end; ++i) {
    LoggedCertificate cert;

    if (!it->GetNextEntry(&cert) || cert.sequence_number() != i) {
      break;
    }

    string leaf_input;
    string extra_data;
    string sct_data;
    if (!cert.SerializeForLeaf(&leaf_input) ||
        !cert.SerializeExtraData(&extra_data) ||
        (include_scts &&
         Serializer::SerializeSCT(cert.sct(), &sct_data) != Serializer::OK)) {
      LOG(WARNING) << "Failed to serialize entry @ " << i << ":\n"
                   << cert.DebugString();
      return output_->SendError(req, HTTP_INTERNAL, "Serialization failed.");
    }

    JsonObject json_entry;
    json_entry.AddBase64("leaf_input", leaf_input);
    json_entry.AddBase64("extra_data", extra_data);

    if (include_scts) {
      // This is non-standard, and currently only used by other SuperDuper log
      // nodes when "following" to fetch data from each other:
      json_entry.AddBase64("sct", sct_data);
    }

    json_entries.Add(&json_entry);
  }

  if (json_entries.Length() < 1) {
    return output_->SendError(req, HTTP_BADREQUEST, "Entry not found.");
  }

  JsonObject json_reply;
  json_reply.Add("entries", json_entries);

  output_->SendJsonReply(req, HTTP_OK, json_reply);
}


void HttpHandler::BlockingAddChain(evhttp_request* req,
                                   const shared_ptr<CertChain>& chain) {
  SignedCertificateTimestamp sct;

  AddChainReply(output_, req,
                CHECK_NOTNULL(frontend_)
                    ->QueueX509Entry(CHECK_NOTNULL(chain.get()), &sct),
                sct);
}

bool HttpHandler::IsNodeStale() const {
  lock_guard<mutex> lock(mutex_);
  return node_is_stale_;
}


void HttpHandler::UpdateNodeStaleness() {
  if (!task_.task()->IsActive()) {
    // We're shutting down, just return.
    return;
  }

  const bool node_is_stale(controller_->NodeIsStale());
  {
    lock_guard<mutex> lock(mutex_);
    node_is_stale_ = node_is_stale;
  }

  event_base_->Delay(seconds(FLAGS_staleness_check_delay_secs),
                     task_.task()->AddChild(
                         bind(&HttpHandler::UpdateNodeStaleness, this)));
}
