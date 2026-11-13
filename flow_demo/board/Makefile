SDK_ROOT ?= ../../..
TOOLCHAIN_ROOT ?= $(SDK_ROOT)/tools/linux/toolchain
RK_TOOLCHAIN_CROSS ?= aarch64-rockchip1240-linux-gnu
RKNN_INCLUDE ?= $(SDK_ROOT)/project/app/rkai/rkai/common/include
RKNN_LIB_DIR ?= $(SDK_ROOT)/output/out/media_out/lib
MEDIA_OUT ?= $(SDK_ROOT)/output/out/media_out

CC := $(TOOLCHAIN_ROOT)/$(RK_TOOLCHAIN_CROSS)/bin/$(RK_TOOLCHAIN_CROSS)-gcc
CXX := $(TOOLCHAIN_ROOT)/$(RK_TOOLCHAIN_CROSS)/bin/$(RK_TOOLCHAIN_CROSS)-g++
TARGET := out/rknn_smoke
SRC := rknn_smoke.c
POSTPROCESS_TARGET := out/rknn_int8_postprocess
POSTPROCESS_SRC := rknn_int8_postprocess.c
YOLO26_TARGET := out/rknn_yolo26_postprocess
YOLO26_SRC := rknn_yolo26_postprocess.c
YOLO26_INFER_TARGET := out/rknn_yolo26_infer
YOLO26_INFER_SRC := rknn_yolo26_infer.c flow_camera.c flow_image.c flow_dma.c flow_yolov8.c
EVENT_SRC := flow_events.cpp ../tracking/src/crossing_engine.cpp \
	$(addprefix ../third_party/botsort/,botsort_tracker.cpp botsort_matching.cpp botsort_kalman.cpp botsort_lapjv.cpp botsort_gmc.cpp)
EVENT_OBJ := $(addprefix out/events/,$(notdir $(EVENT_SRC:.cpp=.o)))
CAMERA_OBJ := $(addprefix out/,$(YOLO26_INFER_SRC:.c=.o))
EVENT_CFLAGS := -std=c++20 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -DSSV_HAS_OPENCV=0 \
	-I. -I../tracking/src -I../third_party/botsort -I../third_party/nlohmann
CAMERA_CFLAGS := -I$(MEDIA_OUT)/include \
	-I$(MEDIA_OUT)/include/gstreamer-1.0 -I$(MEDIA_OUT)/include/glib-2.0 \
	-I$(MEDIA_OUT)/lib/glib-2.0/include -I$(MEDIA_OUT)/include/rkaiq \
	-I$(MEDIA_OUT)/include/rkaiq/uAPI2 -I$(MEDIA_OUT)/include/rkaiq/common \
	-I$(MEDIA_OUT)/include/rkaiq/xcore -I$(MEDIA_OUT)/include/rkaiq/algos
CAMERA_LIBS := -lgstapp-1.0 -lgstvideo-1.0 -lgstallocators-1.0 -lgstbase-1.0 -lgstreamer-1.0 \
	-lgobject-2.0 -lglib-2.0 -lrkaiq -lrockit -lrga -pthread

CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2
CFLAGS += -I$(RKNN_INCLUDE)
LDFLAGS += -L$(RKNN_LIB_DIR) -Wl,-rpath,/oem/usr/lib -Wl,-rpath-link,$(RKNN_LIB_DIR)
LDLIBS += -lrknnrt -lm

.PHONY: all clean info test-image

all: $(TARGET) $(POSTPROCESS_TARGET) $(YOLO26_TARGET) $(YOLO26_INFER_TARGET)

test-image:
	@mkdir -p out
	$(HOST_CC) -std=c11 -Wall -Wextra -Werror -O2 -I. tests/test_flow_image.c flow_image.c -o out/test_flow_image -lm
	./out/test_flow_image

HOST_CC ?= cc

$(TARGET): $(SRC)
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(POSTPROCESS_TARGET): $(POSTPROCESS_SRC)
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -lm

$(YOLO26_TARGET): $(YOLO26_SRC)
	@mkdir -p $(@D)
	$(CC) -std=c11 -Wall -Wextra -Werror -O2 $< -o $@ -lm

out/%.o: %.c flow_camera.h flow_image.h flow_events.h flow_dma.h flow_yolov8.h
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(CAMERA_CFLAGS) -c $< -o $@

vpath %.cpp . ../tracking/src ../third_party/botsort
out/events/%.o: %.cpp flow_events.h flow_image.h ../tracking/src/crossing_engine.hpp
	@mkdir -p $(@D)
	$(CXX) $(EVENT_CFLAGS) -c $< -o $@

$(YOLO26_INFER_TARGET): $(CAMERA_OBJ) $(EVENT_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS) \
		-Wl,-rpath-link,$(MEDIA_OUT)/root/usr/lib $(LDLIBS) $(CAMERA_LIBS)

info:
	@printf 'CC=%s\nRKNN_INCLUDE=%s\nRKNN_LIB_DIR=%s\nTARGET=%s\nPOSTPROCESS_TARGET=%s\nYOLO26_INFER_TARGET=%s\n' \
		'$(CC)' '$(RKNN_INCLUDE)' '$(RKNN_LIB_DIR)' '$(TARGET)' '$(POSTPROCESS_TARGET)' '$(YOLO26_INFER_TARGET)'

clean:
	rm -rf out
