# Copyright (c) 2016, NVIDIA CORPORATION. All rights reserved.
#
# Permission is hereby granted, free of charge, to any person obtaining a
# copy of this software and associated documentation files (the "Software"),
# to deal in the Software without restriction, including without limitation
# the rights to use, copy, modify, merge, publish, distribute, sublicense,
# and/or sell copies of the Software, and to permit persons to whom the
# Software is furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
# DEALINGS IN THE SOFTWARE.

SOURCES += main.c
SOURCES += egl.c
SOURCES += kms.c
SOURCES += utils.c
SOURCES += eglgears.c

HEADERS += egl.h
HEADERS += kms.h
HEADERS += utils.h
HEADERS += eglgears.h

OBJECTS = $(SOURCES:.c=.o)

EGLSTREAMS_KMS_EXAMPLE = eglstreams-kms-example

CFLAGS += -Wall -Wextra
CFLAGS += $(shell pkg-config --cflags egl)
CFLAGS += $(shell pkg-config --cflags gl)
CFLAGS += $(shell pkg-config --cflags libdrm)

LDFLAGS += $(shell pkg-config --libs egl)
LDFLAGS += $(shell pkg-config --libs gl)
LDFLAGS += $(shell pkg-config --libs libdrm)

prefix = /usr
exec_prefix = /usr
bindir = $(exec_prefix)/bin

CC ?= gcc
CCLD ?= gcc

.PHONY: install clean all
all: $(EGLSTREAMS_KMS_EXAMPLE)

%.o: %.c $(HEADERS)
	$(CC) -c $< -o $@ $(CFLAGS)

$(EGLSTREAMS_KMS_EXAMPLE): $(OBJECTS)
	$(CCLD) -o $@ $(OBJECTS) $(LDFLAGS) -lm

install:
	install -d $(DESTDIR)$(bindir)
	install -m 0755 $(EGLSTREAMS_KMS_EXAMPLE) $(DESTDIR)$(bindir)/

clean:
	rm -f *.o $(EGLSTREAMS_KMS_EXAMPLE) *~
