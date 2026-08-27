SDK_ROOT ?= ../../../../..
MEDIA_OUT ?= $(SDK_ROOT)/output/out/media_out
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/tools/linux/toolchain
RK_TOOLCHAIN_CROSS ?= aarch64-rockchip1240-linux-gnu
CC := $(TOOLCHAIN_ROOT)/$(RK_TOOLCHAIN_CROSS)/bin/$(RK_TOOLCHAIN_CROSS)-gcc

TARGET := rockiva_probe
SRC := rockiva_probe.c
HOST_CC ?= cc
HOST_TARGET := rockiva_probe_stub
HOST_SRC := rockiva_probe.c rockiva_stub.c
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CFLAGS += -I$(MEDIA_OUT)/include
HOST_CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
HOST_CFLAGS += -I$(MEDIA_OUT)/include
LDFLAGS += -L$(MEDIA_OUT)/lib -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(MEDIA_OUT)/lib
LDLIBS += -lrockiva -lpthread -ldl -lm

.PHONY: all clean info test

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(HOST_TARGET): $(HOST_SRC)
	$(HOST_CC) $(HOST_CFLAGS) -o $@ $^ -lpthread -lm

test: $(HOST_TARGET)
	./test_probe.sh ./$(HOST_TARGET)

info:
	@printf 'CC=%s\nMEDIA_OUT=%s\n' '$(CC)' '$(MEDIA_OUT)'

clean:
	rm -f $(TARGET) $(HOST_TARGET)
