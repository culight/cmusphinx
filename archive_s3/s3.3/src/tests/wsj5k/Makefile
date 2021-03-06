# ====================================================================
# Copyright (c) 2000 Carnegie Mellon University.  All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions
# are met:
#
# 1. Redistributions of source code must retain the above copyright
#    notice, this list of conditions and the following disclaimer. 
#
# 2. Redistributions in binary form must reproduce the above copyright
#    notice, this list of conditions and the following disclaimer in
#    the documentation and/or other materials provided with the
#    distribution.
#
# This work was supported in part by funding from the Defense Advanced 
# Research Projects Agency and the National Science Foundation of the 
# United States of America, and the CMU Sphinx Speech Consortium.
#
# THIS SOFTWARE IS PROVIDED BY CARNEGIE MELLON UNIVERSITY ``AS IS'' AND 
# ANY EXPRESSED OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, 
# THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
# PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL CARNEGIE MELLON UNIVERSITY
# NOR ITS EMPLOYEES BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
# SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT 
# LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, 
# DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY 
# THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT 
# (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE 
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
#
# ====================================================================
#
#  Sphinx III
#
# ====================================================================
TOP=../../..
DIRNAME=src/tests/wsj5k
BUILD_DIRS =
ALL_DIRS= $(BUILD_DIRS)
# H = cmd_ln_args.h fe_dump.h feat_dump.h new_fe_sp_dump.h live_dump.h metrics.h
# LIVEDECSRCS = parse_args_file.c fe_dump.c feat_dump.c new_fe_sp_dump.c live_dump.c metrics.c
MAINSRCS = dump_frontend.c batch_metrics.c
# OTHERSRCS = main.c
LIVEDECOBJS = $(LIVEDECSRCS:.c=.o) $(BASESRCS:.c=.o) 
FILES = Makefile $(SRCS) $(H)
LIBNAME= tests

LOCAL_INCLUDES = -I$(TOP)/src -I$(TOP)/src/libs3decoder -I$(TOP)/src/libs3audio

ALL = $(BINDIR)/batchmetrics

include $(TOP)/config/common_make_rules

ifeq ($(MODE),quick)
  MODEAFFIX = _quick
else
  MODEAFFIX = 
endif

$(BINDIR)/batchmetrics: ../batch_metrics.o $(LIVEDECOBJS)
	$(CC) $(CFLAGS) -o $@ batch_metrics.o $(LIVEDECOBJS) $(LDFLAGS) $(LIBS) -Wl,-R$(LIBDIR)

# bigram with verbalized punctuation

wsj5k_bigram:
	rm -f gmake-wsj5k_bigram$(MODEAFFIX).results
	$(BINDIR)/batchmetrics /lab/speech/sphinx4/data/wsj0/wsj0/wsj5k$(MODEAFFIX).batch / ./ARGS.wsj5k_bigram > gmake-wsj5k_bigram$(MODEAFFIX).results

# bigram with no verbalized punctuation

wsj5k_bigram_nvp:
	rm -f gmake-wsj5k_bigram$(MODEAFFIX).results
	$(BINDIR)/batchmetrics /lab/speech/sphinx4/data/wsj0/wsj0/wsj5k.e$(MODEAFFIX).batch / ./ARGS.wsj5k_bigram > gmake-wsj5k_bigram$(MODEAFFIX).nvp.results

# trigram with verbalized punctuation
wsj5k_trigram:
	rm -f gmake-wsj5k_trigram$(MODEAFFIX).results
	$(BINDIR)/batchmetrics /lab/speech/sphinx4/data/wsj0/wsj0/wsj5k$(MODEAFFIX).batch / ./ARGS.wsj5k_trigram > gmake-wsj5k_trigram$(MODEAFFIX).results

# trigram with no verbalized punctuation

wsj5k_trigram_nvp:
	rm -f gmake-wsj5k_trigram$(MODEAFFIX).results
	$(BINDIR)/batchmetrics /lab/speech/sphinx4/data/wsj0/wsj0/wsj5k.e$(MODEAFFIX).batch / ./ARGS.wsj5k_trigram > gmake-wsj5k_trigram$(MODEAFFIX).nvp.results
