AC_PREREQ([2.69])
AC_INIT([certificate-transparency], [0.9], [certificate-transparency@googlegroups.com])
AM_INIT_AUTOMAKE([-Wall -Werror foreign subdir-objects])
AC_CONFIG_SRCDIR([cpp/server/ct-server.cc])
AC_CONFIG_HEADERS([cpp/config.h])
AC_CONFIG_MACRO_DIRS([m4])
AM_SILENT_RULES([yes])
AC_LANG([C++])

GMOCK_DIR="${GMOCK_DIR=/usr/src/gmock}"
AC_ARG_VAR([GMOCK_DIR], [directory containing Google Mock])
GTEST_DIR="${GTEST_DIR="$GMOCK_DIR/gtest"}"
AC_ARG_VAR([GTEST_DIR], [directory containing Google Test])

# Checks for programs.
AC_PROG_AWK
AC_PROG_CXX
AC_PROG_CC
AC_PROG_CPP
AC_PROG_LN_S
AC_PROG_RANLIB
AM_PROG_AR
AX_CXX_COMPILE_STDCXX_11
AC_CHECK_PROGS([ANT], [ant])

PKG_CHECK_MODULES([json_c], [json-c])

# Checks for header files.
AC_HEADER_RESOLV
AC_CHECK_HEADERS([arpa/inet.h fcntl.h limits.h netinet/in.h stddef.h stdint.h stdlib.h string.h sys/socket.h sys/time.h unistd.h])
AC_CHECK_HEADER([event2/event.h],,
                [AC_MSG_ERROR([libevent headers could not be found])])

# Check for working GTest/GMock.
saved_CPPFLAGS="$CPPFLAGS"
AS_UNSET([missing_gtest])
AS_UNSET([missing_gmock])
AS_VAR_APPEND([CPPFLAGS], [" -I$GTEST_DIR/include"])
AC_CHECK_HEADER([gtest/gtest.h],, [missing_gtest=1])
AS_VAR_APPEND([CPPFLAGS], [" -I$GMOCK_DIR/include"])
AC_CHECK_HEADER([gmock/gmock.h],, [missing_gmock=1])
AS_VAR_APPEND([CPPFLAGS], [" -I$GTEST_DIR"])
AC_CHECK_HEADER([$GTEST_DIR/src/gtest-all.cc],, [missing_gtest=1])
AS_VAR_APPEND([CPPFLAGS], [" -I$GMOCK_DIR"])
AC_CHECK_HEADER([$GMOCK_DIR/src/gmock-all.cc],, [missing_gmock=1])
CPPFLAGS="$saved_CPPFLAGS"
AS_IF([test -n "$missing_gtest"],
      [AC_MSG_ERROR([could not find a working Google Test])])
AS_IF([test -n "$missing_gmock"],
      [AC_MSG_ERROR([could not find a working Google Mock])])

# Checks for libraries.
AC_SEARCH_LIBS([__b64_ntop], [resolv])

save_LIBS="$LIBS"
AS_UNSET([LIBS])
AC_SEARCH_LIBS([event_base_dispatch], [event],, [missing_libevent=1],
               [$save_LIBS])
AC_SEARCH_LIBS([evthread_use_pthreads], [event_pthreads],, [missing_libevent=1],
               [$save_LIBS])
AC_SUBST([libevent_LIBS], [$LIBS])
AS_IF([test -n "$missing_libevent"],
      [AC_MSG_ERROR([could not find the libevent libraries])])
LIBS="$save_LIBS"

# Checks for typedefs, structures, and compiler characteristics.
AC_TYPE_INT32_T
AC_TYPE_INT64_T
AC_TYPE_MODE_T
AC_TYPE_SIZE_T
AC_TYPE_SSIZE_T
AC_TYPE_UINT16_T
AC_TYPE_UINT32_T
AC_TYPE_UINT64_T
AC_TYPE_UINT8_T

# Checks for library functions.
AC_FUNC_FORK
AC_CHECK_FUNCS([alarm gettimeofday memset mkdir select socket strdup strerror strtol])

# TODO(pphaneuf): We should validate that we have all the tools and
# libraries that we require here, instead of letting the compilation
# fail later during the build (sometimes in ways that are unclear to
# the user.

AM_CONDITIONAL([HAVE_ANT], [test -n "$ANT"])
AC_DEFINE_UNQUOTED([TEST_SRCDIR], ["$srcdir"], [Top of the source directory, for tests.])
AC_CONFIG_FILES([Makefile])
AC_OUTPUT