ifeq ($(APP_PARAM), )
    APP_PARAM:=../../project/app/Makefile.param
endif
include $(APP_PARAM)

export LC_ALL=C
SHELL:=/bin/bash

CURRENT_DIR := $(shell pwd)
SDK_DIR := $(abspath $(CURRENT_DIR)/../..)
APP_STUB_DIR := $(SDK_DIR)/project/app/media_engine

PKG_NAME := media_engine
PKG_BIN ?= out
BEAR ?= bear
COMPILE_COMMANDS ?= $(CURRENT_DIR)/compile_commands.json

GST_STAGE := $(RK_PROJECT_OUTPUT)/media_out

RK_APP_CFLAGS = -I $(RK_APP_MEDIA_INCLUDE_PATH) \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/uAPI \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/uAPI2 \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/algos \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/common \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/xcore \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/iq_parser \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/rkaiq/iq_parser_v2 \
				-I $(RK_APP_MEDIA_INCLUDE_PATH)/libdrm \
				-I $(GST_STAGE)/include/gstreamer-1.0 \
				-I $(GST_STAGE)/include/glib-2.0 \
				-I $(GST_STAGE)/lib/glib-2.0/include \
				-I $(CURRENT_DIR)/src \
				-I $(CURRENT_DIR)/third_party/cJSON \
				-I $(CURRENT_DIR)/third_party/libyaml/include \
				-DHAVE_CONFIG_H=1

RK_APP_LDFLAGS = -L$(RK_APP_MEDIA_LIBS_PATH) \
				 -L$(GST_STAGE)/lib \
				 -Wl,-rpath,/oem/usr/lib \
				 -Wl,-rpath-link,$(RK_APP_MEDIA_LIBS_PATH):$(GST_STAGE)/lib:$(RK_APP_PATH_LIB_INCLUDE)/root/usr/lib \
				 -pthread -lgstreamer-1.0 -lgstbase-1.0 -lgstvideo-1.0 \
				 -lgstallocators-1.0 -lgobject-2.0 -lglib-2.0 -lrga \
				 -lrockit -lrkaiq -lm

SRCS := src/main.c \
		src/common/util.c \
		src/config/config.c \
		src/engine/engine.c \
		src/ipc/ipc_server.c \
		src/session/session_mgr.c \
		src/gst/gst_runner.c \
		src/gst/me_rga_rotate.c \
		src/aiq/aiq_ctrl.c \
		third_party/cJSON/cJSON.c \
		third_party/libyaml/src/api.c \
		third_party/libyaml/src/dumper.c \
		third_party/libyaml/src/emitter.c \
		third_party/libyaml/src/loader.c \
		third_party/libyaml/src/parser.c \
		third_party/libyaml/src/reader.c \
		third_party/libyaml/src/scanner.c \
		third_party/libyaml/src/writer.c

PKG_TARGET := media_engine-build

.PHONY: all media_engine-build compile_commands clean distclean info

ifeq ($(PKG_BIN),)
$(error ### $(CURRENT_DIR): PKG_BIN is NULL, Please Check !!!)
endif

all: $(PKG_TARGET)
	@echo "build $(PKG_NAME) done"

media_engine-build:
	rm -rf $(PKG_BIN); \
	mkdir -p $(PKG_BIN)/bin; \
	for src in $(SRCS); do \
		obj=$(PKG_BIN)/obj/$${src%.c}.o; \
		mkdir -p $$(dirname $$obj); \
		extra=; \
		case "$$src" in third_party/*) extra="-w";; esac; \
		$(RK_APP_CROSS)-gcc -Wall -Wextra -O2 $(RK_APP_CFLAGS) \
			$$extra -c $(CURRENT_DIR)/$$src -o $$obj || exit 1; \
	done; \
	$(RK_APP_CROSS)-gcc -Wall -Wextra -O2 \
		$$(find $(PKG_BIN)/obj -name '*.o') \
		-o $(PKG_BIN)/bin/$(PKG_NAME) \
		$(RK_APP_LDFLAGS)
	$(call MAROC_COPY_PKG_TO_APP_OUTPUT, $(RK_APP_OUTPUT), $(PKG_BIN))
	@if [ "$$(readlink -f "$(PKG_BIN)")" != "$$(readlink -f "$(APP_STUB_DIR)/out")" ]; then \
		mkdir -p $(APP_STUB_DIR)/out; \
		cp -af $(PKG_BIN)/* $(APP_STUB_DIR)/out/; \
	fi

compile_commands:
	@command -v "$(BEAR)" >/dev/null 2>&1 || { \
		echo "error: Bear is required to generate $(COMPILE_COMMANDS)" >&2; \
		exit 1; \
	}
	@$(BEAR) --output "$(COMPILE_COMMANDS)" -- \
		$(MAKE) --no-print-directory media_engine-build
	@echo "generated $(COMPILE_COMMANDS)"

clean:
	@rm -rf $(PKG_BIN)

distclean: clean

info:
	@echo -e "$(C_YELLOW)-------------------------------------------------------------------------$(C_NORMAL)"
	@echo -e "$(CURRENT_DIR) builds media_engine"
	@echo -e "$(C_YELLOW)-------------------------------------------------------------------------$(C_NORMAL)"
