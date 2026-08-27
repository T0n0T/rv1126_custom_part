# RockIVA board probe

This directory contains a board-only validation tool. It is deliberately not
linked into the production `media_engine` target.

The probe reads tightly packed raw NV12 frames from a file, initializes
RockIVA in `ROCKIVA_MODE_VIDEO`, enables the pure detection callback, pushes
the requested number of frames, waits for completion, and prints:

- RockIVA version and selected initialization parameters;
- each push result and frame ID;
- detection status, object ID, state, score, type, normalized rectangle and
  callback frame ID;
- frame-release callback channel, count, frame IDs and memory handles;
- final push/detection/release counts with per-callback latency statistics and
  ownership/channel consistency counters.

The single `probe_state` pointer is passed as `ROCKIVA_Init` userdata and is
used by both callbacks. Each CPU frame is registered as pending before
`ROCKIVA_PushFrame`; a successful push transfers ownership to RockIVA, and the
release callback frees only the matching pointer recorded by this probe. A
push failure frees the buffer only when the callback has not already returned
it. This also makes a synchronous callback during `ROCKIVA_PushFrame` safe.
The detector is released only after a successful all-frame wait. Shutdown
attempts a final wait and always reports the global release result; if that
wait or release fails, the probe deliberately leaves unresolved buffers and
callback state alive until process exit instead of risking a use-after-free.

It intentionally uses a CPU virtual address (`dataAddr`) and a tightly packed
NV12 file first. The capture path's DMA-BUF FD/physical-address lifetime is a
separate board experiment: after this probe is copied to the target, repeat it
with the capture allocator and compare the release callback accounting before
changing production code.

## Cross-compile

From this directory, using the SDK toolchain and staged media output:

```sh
make
```

Override `SDK_ROOT`, `MEDIA_OUT`, `TOOLCHAIN_ROOT`, or
`RK_TOOLCHAIN_CROSS` when the probe is built outside this checkout. The
resulting ELF is AArch64 and must be copied together with `librockiva.so`,
`librknnrt.so`, their runtime dependencies, and the selected model data.

## Host fault-path tests

`make test` builds a host-only executable against `rockiva_stub.c`. The stub is
never linked into the board probe; it drives process-level checks for normal
detect/release callbacks, initialization and callback-registration failures,
empty/short input, push failure, wait failure, and SDK cleanup failures. These
tests validate exit status and CPU-buffer ownership accounting, not RockIVA
inference quality or board behavior.

## Input and run

The input is exactly `width * height * 3 / 2` bytes per frame. For example,
for a `640x360` probe with 30 frames, the file must be 10,368,000 bytes.
The file can contain repeated frames when validating lifecycle and ownership.

```sh
./run_probe.sh
MODEL_PATH=/oem/usr/lib INPUT=/tmp/me/640x360.nv12 \
  WIDTH=640 HEIGHT=360 FRAMES=30 FPS=10 MODEL=pfp ./run_probe.sh
```

Set `ROCKIVA_LIB_DIR` when the runtime libraries are staged outside
`/oem/usr/lib`; it is prepended to `LD_LIBRARY_PATH` by the runner.

The exit status is successful only when initialization and input completed,
all requested pushes succeeded, all waits and SDK cleanup succeeded, and every
pushed frame was returned through the release callback. A successful process
is not T1 evidence by itself: record the complete output, board/firmware/library
versions, NPU/CPU/memory metrics, and the capture-side DMA-BUF experiment in
the checklist.
