## Process this file with automake to produce Makefile.in	-*- makefile -*-
# Description: Makefile for GASNet other/arbiter
# Copyright 2019, Regents of the University of California
# Terms of use are as specified in license.txt

AUTOMAKE_OPTIONS = foreign 1.4

EXTRA_DIST = arb

SUBDIRS = .

if BUILD_ARBITER

ARBITER_SRC = $(srcdir)/arb/src

include_HEADERS = $(ARBITER_SRC)/arb.h

lib_LIBRARIES = libarb.a
libarb_a_SOURCES = arb.c
libarb_a_DEPENDENCIES = arb.c $(include_HEADERS)
libarb_a_CFLAGS = -I$(ARBITER_SRC)

# automake dislikes src and obj not in same dir
BUILT_SOURCES = arb.c
arb.c: $(ARBITER_SRC)/arb.c
	cp $< $@

endif
