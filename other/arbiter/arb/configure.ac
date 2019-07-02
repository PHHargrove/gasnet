#
# See COPYRIGHT in top-level directory.
#

AC_PREREQ(2.62)
m4_include([version.m4])
AC_INIT([Arb], ARB_VERSION_m4)

AC_CONFIG_AUX_DIR(aux)
AC_CONFIG_MACRO_DIR(aux)

AC_PROG_CC
AC_PROG_CC_C99
AM_PROG_CC_C_O

AM_INIT_AUTOMAKE([-Wall -Wno-portability-recursive -Werror foreign 1.12.3 subdir-objects])

AM_PROG_AR

AC_CONFIG_HEADER(src/arb_config.h)

# Non-verbose make
m4_ifdef([AM_SILENT_RULES], [AM_SILENT_RULES([yes])])

LT_INIT

AC_PROG_MKDIR_P
AC_PROG_INSTALL

# Build information
# these values come from the m4_include above
ARB_VERSION="ARB_VERSION_m4"
ARB_RELEASE_DATE="ARB_RELEASE_DATE_m4"
if test -z "$ARB_VERSION" ; then
    AC_MSG_ERROR([Version information not found. Configuration aborted.])
fi
AC_SUBST(ARB_VERSION)

AC_HEADER_STDC

AC_CHECK_LIB([pthread],[pthread_key_create],have_pthreads=yes)

AC_OUTPUT(Makefile)
