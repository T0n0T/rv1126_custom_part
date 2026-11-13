#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
SDK_ROOT=${SDK_ROOT:-$(CDPATH='' cd -- "$SCRIPT_DIR/../../.." && pwd)}
TOOLCHAIN=${RK_TOOLCHAIN_CROSS:-aarch64-rockchip1240-linux-gnu}
CXX=${CXX:-"$SDK_ROOT/tools/linux/toolchain/$TOOLCHAIN/bin/$TOOLCHAIN-g++"}
BUILD_DIR=${BUILD_DIR:-"$SCRIPT_DIR/build-rv1126b"}

if [ ! -x "$CXX" ]; then
	printf 'AArch64 C++ compiler not found: %s\nSet SDK_ROOT or CXX to the RV1126B SDK toolchain.\n' "$CXX" >&2
	exit 1
fi

cmake -S "$SCRIPT_DIR" -B "$BUILD_DIR" \
	-DCMAKE_SYSTEM_NAME=Linux \
	-DCMAKE_SYSTEM_PROCESSOR=aarch64 \
	-DCMAKE_CXX_COMPILER="$CXX" \
	-DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target flow_demo_cli --parallel "${JOBS:-4}"
file "$BUILD_DIR/flow_demo_cli"
