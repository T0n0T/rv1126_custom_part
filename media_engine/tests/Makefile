CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -I../src -I../third_party/libyaml/include -DHAVE_CONFIG_H=1
PKG_CFLAGS := $(shell pkg-config --cflags glib-2.0)
PKG_LIBS := $(shell pkg-config --libs glib-2.0)

all: run

session_mgr_test: session_mgr_test.c ../src/session/session_mgr.c ../src/common/util.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -o $@ $^ $(PKG_LIBS)

LIBYAML_SRCS := ../third_party/libyaml/src/api.c \
	../third_party/libyaml/src/dumper.c \
	../third_party/libyaml/src/emitter.c \
	../third_party/libyaml/src/loader.c \
	../third_party/libyaml/src/parser.c \
	../third_party/libyaml/src/reader.c \
	../third_party/libyaml/src/scanner.c \
	../third_party/libyaml/src/writer.c

config_test: config_test.c ../src/config/config.c ../src/analytics/config.c \
	../src/common/util.c $(LIBYAML_SRCS)
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I../third_party/libyaml/include -o $@ $^ $(PKG_LIBS)

observation_test: observation_test.c ../src/analytics/observation.c
	$(CC) $(CFLAGS) -o $@ $^

run: session_mgr_test config_test observation_test
	./session_mgr_test
	./config_test
	./observation_test

clean:
	rm -f session_mgr_test config_test observation_test
