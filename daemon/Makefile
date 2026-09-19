# Omacall daemon and CLI.
#
# pjproject is built inside third_party/ and linked statically; build.mak is the
# upstream way for an external program to pick up the exact flags that build was
# configured with. There is no libpjproject.pc unless pjproject is installed, so
# pkg-config is not an option here.

PJDIR ?= $(CURDIR)/third_party/pjproject-2.17
include $(PJDIR)/build.mak

# Must match manifest.json and packaging/PKGBUILD; `make version-check` in the
# repository root fails when they drift apart.
VERSION := 0.1.0

PREFIX  ?= /usr
DESTDIR ?=

CJSON_CFLAGS := $(shell pkg-config --cflags libcjson)
CJSON_LIBS   := $(shell pkg-config --libs libcjson)

# _GNU_SOURCE, not -std=gnu11: the project's own code stays C11, but pjproject's
# headers call bzero(), which strict C11 hides behind the POSIX feature macro.
# -MMD -MP writes a .d file per object listing the headers it included, which
# the include below pulls in. Without it, changing a struct in a header
# recompiles only some of the objects: the rest keep the old layout, write past
# the end of a structure the others allocated, and corrupt whatever sits next
# to it. That is a long afternoon to debug and one line to prevent.
OMA_CFLAGS := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 -g -MMD -MP \
              -DOMACALL_VERSION='"$(VERSION)"' $(PJ_CFLAGS) $(CJSON_CFLAGS)
OMA_LIBS   := $(PJ_LDFLAGS) $(PJ_LDLIBS) $(CJSON_LIBS)

DAEMON_SRCS := src/main.c src/config.c src/store.c src/events.c src/ipc.c \
               src/calls.c src/tones.c \
               src/sip.c src/log.c src/util.c
DAEMON_OBJS := $(DAEMON_SRCS:.c=.o)

CLI_SRCS := cli/omacall.c src/util.c
CLI_OBJS := $(CLI_SRCS:.c=.cli.o)

.PHONY: all clean install

all: omacalld omacall

omacalld: $(DAEMON_OBJS)
	$(CC) -o $@ $^ $(OMA_LIBS)

# The CLI only speaks the socket protocol, so it links cJSON but not pjproject.
omacall: $(CLI_OBJS)
	$(CC) -o $@ $^ $(CJSON_LIBS)

src/%.o: src/%.c
	$(CC) $(OMA_CFLAGS) -c -o $@ $<

%.cli.o: %.c
	$(CC) -std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -O2 -g -MMD -MP \
	      -DOMACALL_VERSION='"$(VERSION)"' $(CJSON_CFLAGS) -c -o $@ $<

clean:
	rm -f $(DAEMON_OBJS) $(CLI_OBJS) $(DAEMON_OBJS:.o=.d) $(CLI_OBJS:.o=.d) \
	      omacalld omacall

-include $(DAEMON_OBJS:.o=.d) $(CLI_OBJS:.o=.d)

install: all
	install -Dm755 omacalld $(DESTDIR)$(PREFIX)/bin/omacalld
	install -Dm755 omacall $(DESTDIR)$(PREFIX)/bin/omacall
