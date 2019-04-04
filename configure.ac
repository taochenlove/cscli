#                                               -*- Autoconf -*-
# Process this file with autoconf to produce a configure script.

AC_PREREQ([2.69])
AC_INIT([cscli], [1.0.0], [447181856@qq.com])
AC_CONFIG_SRCDIR([lib/thread.c])
AC_CONFIG_HEADERS([config.h])

AM_INIT_AUTOMAKE([foreign -Wall -Werror])

# Checks for programs.
AC_PROG_CC

# Checks for libraries.
AC_PROG_RANLIB
AM_PROG_AR

# Checks for header files.
AC_CHECK_HEADERS([stdlib.h string.h sys/time.h unistd.h])

# Checks for typedefs, structures, and compiler characteristics.

# Checks for library functions.
AC_FUNC_MALLOC
AC_CHECK_FUNCS([clock_gettime memset select])

AC_ARG_WITH(thread_test, AS_HELP_STRING([--with-thread_test], [support thread test]), [thread_test=yes], [thread_test=no])

AM_CONDITIONAL([ENBALE_THREAD_TEST], [test "$thread_test" = yes])


AC_CONFIG_FILES([Makefile 
                 lib/Makefile 
                 test/Makefile])

AC_OUTPUT
