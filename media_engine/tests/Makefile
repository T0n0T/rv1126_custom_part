CC ?= cc
CFLAGS ?= -Wall -Wextra -O2 -I../src -I../third_party/libyaml/include -DHAVE_CONFIG_H=1
PKG_CFLAGS := $(shell pkg-config --cflags glib-2.0)
PKG_LIBS := $(shell pkg-config --libs glib-2.0)
GST_CFLAGS = $(shell pkg-config --cflags gstreamer-1.0 glib-2.0)
GST_LIBS = $(shell pkg-config --libs gstreamer-1.0 glib-2.0)
RGA_TEST_CFLAGS = $(shell pkg-config --cflags gstreamer-video-1.0 gstreamer-allocators-1.0)
RGA_TEST_LIBS = $(shell pkg-config --libs gstreamer-video-1.0 gstreamer-allocators-1.0)

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

event_engine_test: event_engine_test.c ../src/analytics/event_engine.c \
		../src/analytics/config.c ../src/analytics/observation.c
	$(CC) $(CFLAGS) -o $@ $^

event_journal_test: event_journal_test.c ../src/analytics/event_journal.c \
		../src/analytics/event_codec.c ../src/analytics/event_engine.c \
		../src/analytics/config.c ../src/analytics/observation.c \
		../src/common/util.c ../third_party/cJSON/cJSON.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I../third_party/cJSON -o $@ $^ \
		$(PKG_LIBS) -lm

evidence_cache_test: evidence_cache_test.c ../src/analytics/evidence_cache.c \
		../src/analytics/event_engine.c ../src/analytics/config.c \
		../src/analytics/observation.c ../third_party/cJSON/cJSON.c
	$(CC) $(CFLAGS) $(PKG_CFLAGS) -I../third_party/cJSON -o $@ $^ \
		$(PKG_LIBS) -lm

gst_runner_lifecycle_test: gst_runner_lifecycle_test.c ../src/gst/gst_runner.c
	$(CC) $(CFLAGS) $(GST_CFLAGS) -ffunction-sections -fdata-sections \
		-o $@ $< $(GST_LIBS) -Wl,--gc-sections

me_rga_allocation_test: me_rga_allocation_test.c ../src/gst/me_rga_rotate.c \
		../src/gst/me_rga_rotate.h stubs/rga/RgaApi.h stubs/rga/drmrga.h
	$(CC) $(CFLAGS) $(RGA_TEST_CFLAGS) -Istubs -o $@ $< $(RGA_TEST_LIBS)

run: session_mgr_test config_test observation_test event_engine_test \
		event_journal_test evidence_cache_test
	./session_mgr_test
	./config_test
	./observation_test
	./event_engine_test
	./event_journal_test
	./evidence_cache_test

# This host-only GStreamer test is kept separate from the GLib-only suite.
gst-runner-lifecycle: gst_runner_lifecycle_test
	./gst_runner_lifecycle_test

rga-allocation: me_rga_allocation_test
	G_DEBUG=fatal-criticals ./me_rga_allocation_test

gst-tests: rga-allocation gst-runner-lifecycle

clean:
	rm -f session_mgr_test config_test observation_test event_engine_test \
		event_journal_test evidence_cache_test gst_runner_lifecycle_test \
		me_rga_allocation_test
