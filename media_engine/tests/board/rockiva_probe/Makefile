SDK_ROOT ?= ../../../../..
MEDIA_OUT ?= $(SDK_ROOT)/output/out/media_out
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/tools/linux/toolchain
RK_TOOLCHAIN_CROSS ?= aarch64-rockchip1240-linux-gnu
CC := $(TOOLCHAIN_ROOT)/$(RK_TOOLCHAIN_CROSS)/bin/$(RK_TOOLCHAIN_CROSS)-gcc

TARGET := rockiva_probe
SRC := rockiva_probe.c
DMABUF_TARGET := rockiva_dmabuf_probe
DMABUF_SRC := rockiva_dmabuf_probe.c
V4L2_TARGET := v4l2_expbuf_probe
V4L2_SRC := v4l2_expbuf_probe.c
V4L2_ROCKIVA_TARGET := v4l2_rockiva_probe
V4L2_ROCKIVA_SRC := v4l2_rockiva_probe.c
HOST_CC ?= cc
HOST_TARGET := rockiva_probe_stub
HOST_SRC := rockiva_probe.c rockiva_stub.c
HOST_GUARD_TARGET := mainpath_guard_test
HOST_GUARD_SRC := mainpath_guard_test.c
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CFLAGS += -I$(MEDIA_OUT)/include
HOST_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
HOST_CFLAGS += -I$(MEDIA_OUT)/include
LDFLAGS += -L$(MEDIA_OUT)/lib -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(MEDIA_OUT)/lib
LDLIBS += -lrockiva -lpthread -ldl -lm
GST_CFLAGS := -I$(MEDIA_OUT)/include/gstreamer-1.0 \
	-I$(MEDIA_OUT)/include/glib-2.0 \
	-I$(MEDIA_OUT)/lib/glib-2.0/include
GST_LDFLAGS := -L$(MEDIA_OUT)/lib \
	-Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(MEDIA_OUT)/lib
GST_LDLIBS := -lgstapp-1.0 -lgstvideo-1.0 -lgstallocators-1.0 \
	-lgstbase-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0

.PHONY: all clean info test test-runners test-mainpath-guard

all: $(TARGET) $(DMABUF_TARGET) $(V4L2_TARGET) $(V4L2_ROCKIVA_TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(DMABUF_TARGET): $(DMABUF_SRC)
	$(CC) $(CFLAGS) $(GST_CFLAGS) $< -o $@ $(LDFLAGS) $(GST_LDFLAGS) \
		$(GST_LDLIBS) $(LDLIBS)

$(V4L2_TARGET): $(V4L2_SRC)
	$(CC) $(CFLAGS) $< -o $@

$(V4L2_ROCKIVA_TARGET): $(V4L2_ROCKIVA_SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(HOST_TARGET): $(HOST_SRC)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^ -lpthread -lm

$(HOST_GUARD_TARGET): $(HOST_GUARD_SRC) mainpath_guard.h
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $(HOST_GUARD_SRC)

test: $(HOST_TARGET) $(HOST_GUARD_TARGET) test-runners
	./test_probe.sh ./$(HOST_TARGET)
	./$(HOST_GUARD_TARGET)

test-runners:
	sh ./test_runner_guards.sh

test-mainpath-guard: $(HOST_GUARD_TARGET)
	./$(HOST_GUARD_TARGET)

info:
	@printf 'CC=%s\nMEDIA_OUT=%s\nGST_CFLAGS=%s\n' '$(CC)' '$(MEDIA_OUT)' '$(GST_CFLAGS)'

clean:
	rm -f $(TARGET) $(DMABUF_TARGET) $(V4L2_TARGET) $(V4L2_ROCKIVA_TARGET) \
		$(HOST_TARGET) $(HOST_GUARD_TARGET)
