# RockIVA board probe

This directory contains a board-only validation tool. It is deliberately not
linked into the production `media_engine` target.

The raw-NV12 `rockiva_probe` reads tightly packed raw NV12 frames from a file,
initializes RockIVA in `ROCKIVA_MODE_VIDEO`, enables the pure detection callback, pushes
the requested number of frames (or runs until a stop signal in continuous
mode), waits for completion, and prints (with a selectable verbosity). The
`v4l2_rockiva_probe` executable additionally supports native V4L2 capture and
the MP4 test-source mode described below.

- RockIVA version and selected initialization parameters;
- each push result and frame ID at the default `all` log level;
- detection status, object ID, state, score, type, normalized rectangle and
  callback frame ID at the default `all` log level;
- person observations split by `FIRST`, `TRACKING`, `LOST` and `DISAPPEAR`;
- frame-release callback channel, count, frame IDs and memory handles;
- final push/detection/release counts with per-callback latency statistics and
  ownership/channel consistency counters.

The single `probe_state` pointer is passed as `ROCKIVA_Init` userdata and is
used by both callbacks. Each CPU frame is registered as pending before
`ROCKIVA_PushFrame`; a successful push transfers ownership to RockIVA, and the
release callback frees only the matching pointer recorded by this probe. A
push failure frees the buffer only when the callback has not already returned
it. This also makes a synchronous callback during `ROCKIVA_PushFrame` safe.
`ROCKIVA_WaitFinish` has three outcomes. A successful return is the normal
completion proof. `ROCKIVA_RET_UNSUPPORTED` is an explicit capability fallback:
the probe uses a bounded condition-variable wait and continues only after every
accepted frame has both a completed detection callback and a matching release
callback. Only then are `ROCKIVA_DETECT_Release` and `ROCKIVA_Release` called.
Any other nonzero wait result, a callback timeout, or inconsistent callback
accounting remains a failure. The fallback is not a waiver for push,
detection, release, threshold, or other existing error gates.

If completion cannot be proven, the probe leaves unresolved buffers and
callback state alive until process exit instead of risking a use-after-free.

The native V4L2 probes perform the mainpath guard twice: once before opening
the requested path and once after opening it by comparing the opened
descriptor's character-device identity with `/dev/video24`. If the opened
identity cannot be verified, the probe fails closed. The GStreamer DMA-BUF
probe keeps its pre-open path guard because `v4l2src` owns the capture open;
it remains an independent experiment and must be run only on the unused
selfpath. Its appsink allocation callback requests a single-memory downstream
DMA-BUF pool backed by `/dev/dma_heap/system-uncached` (falling back to
`/dev/dma_heap/system`) so `v4l2src` can copy a multi-plane capture into the
single `dataFd` layout accepted by RockIVA. This is a test-only copy path; it
does not change the production allocator or prove that the production
`media_engine` branch can use the same layout.

The file-input runner sets `MIN_PERSON=1` and `MIN_TRACKING=1` by default. The
native V4L2 runner deliberately defaults both thresholds to zero so that an
empty-scene lifecycle check is possible; set both to `1` when checking a live
person. `MIN_PERSON` and `MIN_TRACKING` may raise the thresholds. Setting either
threshold to zero is accepted only as an explicit empty-scene/lifecycle
diagnostic and cannot satisfy the T1 person and tracking gate.

This threshold gate proves only that person and tracking observations were
reported. It does not prove that `FIRST` and `TRACKING` belong to the same
`objId`, that an ID remains stable through the clip, or that the ID-switch rate
is acceptable. Record those properties from representative board runs before
completing T1.

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
detect/release callbacks, version/initialization/callback-registration failures,
empty/short input, push failure, missing person/tracking observations, the
unsupported-wait fallback with complete and missing release callbacks, wait
failure, and SDK cleanup failures. These
tests validate exit status and CPU-buffer ownership accounting, not RockIVA
inference quality or board behavior.

`make test` also runs `test_runner_guards.sh`. It copies the board runner
scripts and a fake probe into a temporary directory, so the checks never
open a V4L2 node. The checks cover required environment variables, invalid
`ALLOW_MAINPATH` values, safe default argument forwarding, explicit
`--allow-mainpath` forwarding, and the `LD_LIBRARY_PATH`/`GST_PLUGIN_PATH`
environment setup, including the MP4 decoder plugin scanner path. This is a
host-side runner contract test only; it does not prove the target device or
RockIVA runtime is available.

## Input and run

The input is exactly `width * height * 3 / 2` bytes per frame. For example,
for a `640x360` probe with 30 frames, the file must be 10,368,000 bytes.
Use a representative clip containing a person for the default success gate.
Repeated or synthetic empty frames are useful only when validating format and
ownership with the minimum-observation checks deliberately disabled; they are
not detection or tracking evidence.

```sh
./run_probe.sh
MODEL_PATH=/oem/usr/lib INPUT=/tmp/me/640x360.nv12 \
  WIDTH=640 HEIGHT=360 FRAMES=30 FPS=10 MODEL=pfp ./run_probe.sh
```

Set `ROCKIVA_LIB_DIR` when the runtime libraries are staged outside
`/oem/usr/lib`; it is prepended to `LD_LIBRARY_PATH` by the runner.

The exit status is successful only when initialization and input completed,
all requested pushes succeeded, every pushed frame was returned through the
release callback, and either all waits/SDK cleanup succeeded or an SDK
explicitly reported `ROCKIVA_RET_UNSUPPORTED` and the bounded callback
completion fallback proved the same CPU input lifecycle before cleanup. A
fallback success relies only on all callbacks observed by this probe and a
successful SDK release; it does not establish DMA-BUF ownership or lifetime.
DMA-BUF validation remains an independent board gate. A successful process is
not T1 evidence by itself: record the complete output, board/firmware/library
versions, NPU/CPU/memory metrics, and the capture-side DMA-BUF experiment in
the checklist.

## Kernel V4L2 EXPBUF capability probe

`v4l2_expbuf_probe` is a separate test-only AArch64 executable for checking
the kernel exporter without involving GStreamer or RockIVA. It opens the
explicit capture node, requests `V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE` with
progressive NV12, accepts the driver's equivalent `NV12` or `NV12M` result,
allocates `V4L2_MEMORY_MMAP` buffers, runs `VIDIOC_QUERYBUF`, and runs
`VIDIOC_EXPBUF` for every returned physical plane. Each exported fd is
validated with `fcntl(F_GETFD)` and `fstat()`, then closed immediately. It
does not queue buffers, start streaming, read frames, run inference, or claim
T1 readiness.

The device, width, height and requested MMAP buffer count are required. The
probe rejects `/dev/video24` by default; `ALLOW_MAINPATH=1` is required for an
explicitly approved mainpath experiment. Start with an unused selfpath node:

```sh
make v4l2_expbuf_probe SDK_ROOT=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
DEVICE=/dev/video25 WIDTH=640 HEIGHT=360 FRAMES=4 \
  ./run_v4l2_expbuf_probe.sh
```

Before setting its requested format, the probe records the node's current
format. The cleanup path attempts `VIDIOC_STREAMOFF`, releases the queue with
`VIDIOC_REQBUFS(count=0)`, restores that original format, and closes the V4L2
device last. Since this capability probe never starts streaming, an `EINVAL`
from `STREAMOFF` is reported as the expected already-stopped state; other
ioctl, format-restore, or close failures fail the run. A returned buffer count
may be smaller than requested and is reported. A successful
`V4L2_EXPBUF_CAPABILITY_OK` result reports the number of physical planes and
sets `single_fd_candidate=1` only for a one-plane layout. A two-plane `NV12M`
result proves only that the node exported one DMA-BUF descriptor per plane; it
is not directly compatible with RockIVA's single `dataFd`. Neither result
proves that `v4l2src` will expose `GstDmaBufMemory`, that a GStreamer caps
feature will negotiate, that `/dev/video24` is safe to share,
or that RockIVA can consume the exported layout.

## Native V4L2-to-RockIVA lifecycle probe

`v4l2_rockiva_probe` is the next independent board experiment. It owns an
explicit V4L2 `MMAP` capture queue, exports one DMA-BUF fd for each returned
buffer, and passes that fd to `ROCKIVA_PushFrame` with CPU and physical
addresses unset. It accepts only progressive NV12/NV12M whose negotiated
physical plane count is one. An NV12M result with two physical planes is
rejected before RockIVA because `RockIvaImage` has only one `dataFd`; the
strict `sizeimage`/`bytesperline` check can also reject drivers whose trailing
allocation padding does not describe an unambiguous NV12 `hstride`.

The exported fd remains open while RockIVA owns the frame. The probe does not
`VIDIOC_QBUF` a capture buffer until the release callback returns a matching
`frameId`, channel and fd with unset CPU/physical pointers. Cleanup then runs
in order: callback completion, released-buffer requeue, `STREAMOFF`, unmap,
`REQBUFS(0)`, retained-fd close, original-format restore, and device close.
Each `VIDIOC_DQBUF` must report one physical plane, zero `data_offset`, a
nonzero plane length covering the negotiated NV12 allocation, and `bytesused`
within that returned and queried length. If `STREAMOFF` fails, the probe does
not unmap, release the queue, restore the format, or close the device; it leaves
the ownership graph intact until process exit and reports the run as incomplete.
When callback completion cannot be proven, the V4L2 queue, mappings and fd
table remain alive until process exit rather than being torn down underneath
the SDK. This is a lifecycle experiment, not a GStreamer negotiation test or
a production mainpath integration.

Build and run it against an unused selfpath node first:

```sh
make v4l2_rockiva_probe SDK_ROOT=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
DEVICE=/dev/video25 MODEL_PATH=/oem/usr/lib WIDTH=640 HEIGHT=360 FRAMES=30 \
  ROCKIVA_LIB_DIR=/oem/usr/lib MIN_PERSON=1 MIN_TRACKING=1 \
  ./run_v4l2_rockiva_probe.sh
```

For a long-running capture, set `CONTINUOUS=1` and stop it with `SIGINT` or
`SIGTERM`. Continuous mode does not require or forward `FRAMES`, and it always
rejects `/dev/video24`, including path aliases. Frame IDs are still supplied to
RockIVA for callback correlation, but the probe keeps only one fixed record per
granted V4L2 buffer slot; it never grows a table with the run length. On stop,
capture stops and SDK callbacks are drained with the existing per-run timeout
before buffers, mappings, retained DMA-BUF fds and the original format are
cleaned up. If callback completion cannot be proven, teardown remains deferred
until process exit as in the finite probe.

The same `v4l2_rockiva_probe` executable also accepts an MP4 test source. In
this mode RockIVA does not parse the container: the board GStreamer pipeline
uses `filesrc ! qtdemux ! h264parse ! mppvideodec`, then tees the decoded video
into two branches. The analysis branch uses
`videoconvert ! videoscale ! videorate ! video/x-raw,format=NV12` before the
probe's `appsink`; the display branch reuses the production `media_engine`
path, `rgarotate ! kmssink`, for the board screen. The probe copies each
analysis sample to a tightly packed CPU buffer before calling
`ROCKIVA_PushFrame`. The mode shares the RockIVA DET callbacks, person-state
output and ownership checks with the native demo, but its analysis lifecycle is
CPU-buffer based rather than V4L2 DMA-BUF.

MP4 display is enabled by default and uses the same board values as
`media_engine`: KMS connector `97`, plane `75`, and a `480x800` RGA output.
Set `DISPLAY_OUTPUT=0` to run inference without opening the display branch.
`CONNECTOR_ID`, `PLANE_ID`, `PREVIEW_ROTATION`, `PREVIEW_WIDTH` and
`PREVIEW_HEIGHT` override the display settings when needed. A display-enabled
run requires the board's `kmssink`, `librga` and `/dev/rga` runtime path.

`real_time.mp4` is not tracked in this checkout. Set `INPUT` to an H.264 MP4
that is available on the host, copy it to the board, and run a finite sample
test. The current workspace example uses
`/home/Tiger/Documents/rtsp_demo/test1.mp4`:

```sh
adb push /home/Tiger/Documents/rtsp_demo/test1.mp4 /tmp/me/test1.mp4
SOURCE=mp4 INPUT=/tmp/me/test1.mp4 MODEL_PATH=/oem/usr/lib \
  ROCKIVA_LIB_DIR=/oem/usr/lib WIDTH=640 HEIGHT=640 FPS=10 FRAMES=30 \
  MIN_PERSON=1 MIN_TRACKING=1 ./run_v4l2_rockiva_probe.sh
```

For `SOURCE=mp4`, `INPUT` defaults to `/tmp/me/real_time.mp4`, the sample
dimensions default to `640x640`, `FPS` to `10`, and `FRAMES` to `30`. The
runner enables the `rgarotate ! kmssink` screen branch by default, sets
`GST_PLUGIN_PATH` below `ROCKIVA_LIB_DIR` and defaults
`GST_PLUGIN_SCANNER` to `/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner`.
MP4 mode is finite and cannot use `CONTINUOUS` or `ALLOW_MAINPATH`.

For the board runner, a low-noise interactive session is:

```sh
DEVICE=/dev/video25 MODEL_PATH=/oem/usr/lib ROCKIVA_LIB_DIR=/oem/usr/lib \
  WIDTH=640 HEIGHT=360 CONTINUOUS=1 LOG_LEVEL=events \
  REPORT_INTERVAL_MS=1000 /tmp/run_v4l2_rockiva_probe.sh
```

Press `Ctrl-C` once to request a bounded, orderly shutdown. Use
`LOG_LEVEL=summary` for periodic counters without person state transitions, or
`LOG_LEVEL=quiet` when only errors and the final summary are needed.

`BUFFERS`, `MODEL`, `CHANNEL`, `CORE_MASK`, `TIMEOUT_MS`, `LOG_LEVEL`,
`REPORT_INTERVAL_MS`, `MIN_PERSON` and `MIN_TRACKING` are optional runner
variables. `LOG_LEVEL` accepts `quiet`, `summary`, `events` or `all` (the
default). `all` retains the original per-frame callback output. `events` keeps
setup/cleanup and periodic summaries plus person state transitions with target
details; `summary` suppresses per-frame and unchanged-target output; `quiet`
emits errors and the final summary only. `REPORT_INTERVAL_MS` controls periodic
summary output and defaults to 5000 ms; set it to `0` to disable periodic
reports. stdout is line-buffered so reports remain visible during a continuous
run. Object states are printed with readable names (`FIRST`, `TRACKING`, `LOST`,
`DISAPPEAR`, and `NONE`) as well as their numeric SDK value.

The `callback_frame_id` in each `person_event` line is the enclosing detection
result's `frameId`, which is also used for callback correlation. The
per-object `frameId` is emitted separately as `object_frame_id` and may be zero
on the board.

## Analyze a saved T1 log

`analyze_t1_log.sh` is a host-only POSIX shell/AWK report tool. It does not
open a V4L2 node, load RockIVA, or change board state. Give it the saved probe
log, or use `-` to read stdin:

```sh
./analyze_t1_log.sh /tmp/t1-events-v5.log
cat /tmp/t1-events-v5.log | ./analyze_t1_log.sh -
```

The report repeats the final non-periodic `summary captures=...` line (both the
current `mode=...` form and older summaries without `mode` are accepted), extracts the
capture/push/detect/release and sequence/capture/release error fields, counts
unique `obj_id` values only from `person_event` lines, and prints transition
counts, per-ID first/last `callback_frame_id`, event counts, and the longest ID
span. `person` and `tracking` totals from the final summary are reported as
observation totals and are never used as unique-person counts. Per-ID
`observations` counts `object` lines when an `all`-level log contains them; an
`events`-level log has `observations=not-recorded` because unchanged object
observations were intentionally not printed.

The analyzer fails if the final summary is absent or if no `person_event` line
is present. Use `--allow-empty` only for a deliberately empty-scene log; the
final summary is still required:

```sh
./analyze_t1_log.sh --allow-empty /tmp/t1-empty.log
```

The V4L2 runner defaults `MIN_PERSON=0` and `MIN_TRACKING=0` for an explicit
empty-scene/lifecycle diagnostic; set both to `1` when checking a live person.
Zero thresholds do not prove person detection or tracking. The `/dev/video24`
production mainpath is rejected unless `ALLOW_MAINPATH=1` is set for an
explicitly approved finite experiment; continuous mode rejects that override.
A successful build, capability probe or native process does not establish T1
detection quality, stable `objId` tracking, DMA-BUF compatibility with the
production pipeline, or main-video isolation; record those only from
representative board runs.

## Camera DMA-BUF probe

`rockiva_dmabuf_probe` is a separate AArch64 test executable. It creates its
own GStreamer pipeline equivalent to:

```text
v4l2src device=PATH io-mode=dmabuf !
  video/x-raw,format=NV12,width=W,height=H,framerate=FPS/1 ! appsink
```

The V4L2 device, model directory, dimensions, frame count and frame rate are
required arguments. There is no implicit capture device. `/dev/video24` is
rejected by default because it is the production mainpath; `--allow-mainpath`
is required for an explicitly approved experiment. Use the unused selfpath
device first, for example:

```sh
make rockiva_dmabuf_probe SDK_ROOT=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
DEVICE=/dev/video25 MODEL_PATH=/oem/usr/lib WIDTH=640 HEIGHT=360 \
  FRAMES=30 FPS=10 ROCKIVA_LIB_DIR=/oem/usr/lib \
  ./run_dmabuf_probe.sh
```

The probe accepts only one `GstBuffer` memory backed by one DMA-BUF fd, NV12,
progressive dimensions, a zero Y-plane offset, matching positive Y/UV strides,
and a contiguous UV offset described by `stride * hstride`. NV12 has two
logical planes, but this probe requires both planes to reside in that one
physical DMA-BUF memory because `RockIvaImage` has only one `dataFd`. The
complete Y plus UV layout must fit both the `GstMemory` and the `GstBuffer`.
When present, `GstVideoMeta` must describe two logical NV12 planes; otherwise
the plane 0/1 offsets and strides from `GstVideoInfo` are used. The held
GStreamer buffer reference is registered before `ROCKIVA_PushFrame` and is
unreffed only after the release callback returns the matching frame ID and fd.
The callback uses `RockIvaDetectResult.frameId` for detection association; it
does not use `RockIvaObjectInfo.frameId`.

As with the CPU probe, `ROCKIVA_WaitFinish == ROCKIVA_RET_UNSUPPORTED` is
accepted only after a bounded condition-variable wait proves detection and
matching release callbacks for every accepted frame. The appsink pipeline is
stopped only after SDK ownership and callback accounting are proven complete;
on an unsafe failure path it remains referenced until process exit to avoid a
DMA-BUF use-after-free. The summary and exit status include sample validation,
push, detection, release and SDK teardown results.

The DMA-BUF runner defaults `MIN_PERSON=0` and `MIN_TRACKING=0` because this is
a memory/lifetime experiment. Zero thresholds do not provide detection,
tracking, accuracy, ID stability or event-quality evidence. A successful
selfpath run proves only that this independent camera exporter can be passed to
RockIVA; it does not prove that `/dev/video24` or the PID 576 production
`media_engine` buffers can be shared. No host fault test is provided for this
target: a synthetic host buffer would not exercise the V4L2 exporter and would
make the DMA-BUF result misleading.

### MP4 decoder-output mode

`rockiva_dmabuf_probe` also accepts an MP4 input with `--input`. This is a
separate decoder-output experiment from the camera mode above. The board
pipeline is:

```text
filesrc location=PATH ! qtdemux ! h264parse !
  mppvideodec(dma-feature=true) ! video/x-raw,format=NV12,width=W,height=H ! appsink
```

`mppvideodec` is the board decoder and the appsink allocation callback requests
a single DMA-BUF-backed buffer. The probe passes that fd directly to
`ROCKIVA_PushFrame`; it does not copy the sample to CPU memory and it does not
open a V4L2 node. The MP4 DMA-BUF mode has no display branch. Use the
`v4l2_rockiva_probe` MP4 mode above when the decoded clip must also be shown on
the board screen through the reused `rgarotate ! kmssink` path.

The runner defaults for this mode are `INPUT=/tmp/me/test1.mp4`, `768x432`,
`FRAMES=30`, `FPS=10`, `MIN_PERSON=1` and `MIN_TRACKING=1`. The clip must already
be present on the board:

```sh
make rockiva_dmabuf_probe SDK_ROOT=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
adb push /home/Tiger/Documents/rtsp_demo/test1.mp4 /tmp/me/test1.mp4
SOURCE=mp4 INPUT=/tmp/me/test1.mp4 MODEL_PATH=/oem/usr/lib \
  ROCKIVA_LIB_DIR=/oem/usr/lib WIDTH=768 HEIGHT=432 FPS=10 FRAMES=30 \
  MIN_PERSON=1 MIN_TRACKING=1 ./run_dmabuf_probe.sh
```

In this mode `FPS` is retained as a requested/recorded probe parameter; the
decoder-output pipeline does not use `videorate` to pace playback. This run
therefore validates frame delivery and RockIVA ownership, not real-time
throughput or an end-to-end 10 FPS budget.

The final `mode=mp4-dmabuf` summary must show equal `samples_received`,
`pushed`, `detection_callbacks` and `released_frames`, zero lifecycle/error
counters, and at least the requested person/tracking observations. A successful
run proves the board decoder-to-RockIVA DMA-BUF lifetime for this test source;
it does not prove production `media_engine` integration, display output, image
orientation, tracking-ID stability or event-engine delivery.

The target links the staged `gstreamer-1.0`, `gstapp-1.0`, `gstvideo-1.0`,
`gstallocators-1.0`, GLib, RockIVA and RGA libraries. The board also needs the
matching GStreamer `mppvideodec`, `kmssink` and v4l2 plugins; set
`GST_PLUGIN_PATH` explicitly if they are not under
`${ROCKIVA_LIB_DIR}/gstreamer-1.0`.
