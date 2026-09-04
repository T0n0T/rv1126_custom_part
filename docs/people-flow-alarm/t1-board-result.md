# T1 RV1126B RockIVA 探针结果

日期：2026-08-28、2026-08-31、2026-09-01、2026-09-04（Asia/Shanghai）

## 范围与输入

本记录覆盖非产品探针的 CPU 地址 NV12、原生 V4L2 DMA-BUF 输入和 RockIVA DET
回调；未改动 `media_engine`、daemon 或板端持久配置。CPU 输入来自已目视确认包含施工人员的
`/home/Tiger/Documents/code/agent/yolo_bytetrack/data/input.mp4` 的 00:00:01
起始片段，按以下参数生成：

```sh
ffmpeg -ss 1 -i input.mp4 -vf 'fps=10,scale=640:360:flags=lanczos' \
  -frames:v 30 -pix_fmt nv12 -f rawvideo person-640x360-30.nv12
```

输入为 `640x360`、NV12、30 帧、10 FPS、10,368,000 字节；SHA-256 为
`a499b7513ab67cf307ab02995d7cb5f3e49058f54bf2e17bd290928071c9ee21`。

## 板端指纹

| 项目 | 实测值 |
| --- | --- |
| 连接 | `192.168.1.63:5555`，ADB `device` |
| 型号 | `Rockchip RV1126B XIAOYU 50IPC V10 Board` |
| 内核 | `Linux 6.1.157`，AArch64 |
| `librockiva.so` | 667,552 bytes, MD5 `eca803ba198217923e92bc7c3bfbf32f` |
| `librknnrt.so` | 7,726,232 bytes, MD5 `a37ee1d5d664c79836bf6e35b7ef6289` |
| PFP model | 3,549,849 bytes, MD5 `b74a11b0e23ec68e0a92be5a3802afb4` |
| CLS8 model | 5,206,252 bytes, MD5 `f279177ad3f0e5c8ed1ef1c7f00d30e5` |

`ROCKIVA_GetVersion` 返回成功但未填充版本字符串，因此以上运行时文件指纹是
本次可复现的库/模型标识。

## 探针结果

每组均使用 `MIN_PERSON=1`、`MIN_TRACKING=1`。探针逐帧检查 DET callback、
release callback、frame ID、CPU 指针匹配和 SDK 收尾；直接保存 probe 进程退出码，
不以 ADB transport 状态代替。

| Model | core mask | 直接退出码 | person states | detect avg | release avg | 收尾 |
| --- | --- | --- | --- | --- | --- | --- |
| PFP | `0x0` | 0 | 88 (first 6, tracking 64, lost 14, disappear 4) | 17.525 ms | 17.572 ms | 30/30 detect, 30/30 release, `DETECT_Release`/`Release` 均成功 |
| PFP | `0x4` | 0 | 88 (first 6, tracking 64, lost 14, disappear 4) | 18.831 ms | 18.878 ms | 30/30 detect, 30/30 release, `DETECT_Release`/`Release` 均成功 |
| CLS8 | `0x0` | 0 | 74 (first 5, tracking 55, lost 11, disappear 3) | 29.716 ms | 29.760 ms | 30/30 detect, 30/30 release, `DETECT_Release`/`Release` 均成功 |

本板库的 `ROCKIVA_WaitFinish` 始终返回 `-5` (`ROCKIVA_RET_UNSUPPORTED`)；这不是
检测失败。探针将其显式建模为能力降级，仅在每个已接收帧都同时得到 callback
frame ID 对应的 DET 完成和匹配的 release callback 后，才调用
`ROCKIVA_DETECT_Release` 与 `ROCKIVA_Release`。三组的两个 callback 完成计数均为
`pushed=30 detection_completed=30 released=30`。

PFP 的 `objId/state` 在片段中可见 `FIRST`、`TRACKING`、`LOST`、`DISPEAR` 转换；
callback 层 `RockIvaDetectResult.frameId` 为 1--30。该板库填充的
`RockIvaObjectInfo.frameId` 均为 0，因此后续 T3 必须以结果 callback 的 frame ID
作为输入帧关联键，不能以对象内部 frame ID 代替。

## 隔离节点原生 V4L2 DMA-BUF 生命周期

2026-08-31 在同一板卡的隔离节点 `/dev/video25` 执行了原生
`MMAP + VIDIOC_EXPBUF + RockIVA` 探针。V4L2 查询到的起始格式为 `640x360`、
驱动原始 fourcc `'NM12'`（多平面 NV12，两个物理平面）；这不是单物理平面的
`NV12`。执行前通过 ADB 确认生产节点 `/dev/video24` 未被探针打开、停止或重配。
本次使用 PFP、`coreMask=0x0`、30 帧，`MIN_PERSON=0`、`MIN_TRACKING=0`，因为隔离
节点画面没有人员，阈值为零只用于生命周期诊断。

探针将请求格式协商为 fourcc `'NV12'` 的单物理平面格式
（`bytesperline=640`、`sizeimage=345600`），
4 个 V4L2 MMAP buffer 均成功导出并保持 DMA-BUF fd。30 帧均完成 DQBUF、
`ROCKIVA_PushFrame`、DET callback 和匹配的 release callback；输出计数为
`captures=30`、`pushed=30`、`detected=30`、`released=30`、`sequence_errors=0`、
`release_mismatches=0`。`ROCKIVA_WaitFinish` 返回 `-5` 时，受限 callback fallback
确认 `detection_completed=30`、`released=30`；`ROCKIVA_DETECT_Release`、
`ROCKIVA_Release`、`VIDIOC_REQBUFS(0)`、DMA-BUF fd 关闭以及原格式恢复均成功，
探针直接退出码为 0。

这次运行未观察到 person/tracking（`person=0 tracking=0`），因此只证明隔离采集
导出器与 RockIVA 的内存/释放生命周期可行，不证明检测质量或生产链路可共享。该次
运行结束后 ADB 连接中断，未能取得 PID、主路径 sequence 和 `/dev/video25` 格式的
后置只读快照；后续连接恢复后的复测记录见下节。

## 原生 V4L2->RockIVA 有人场景尝试与停止/重启边界

2026-08-31 在同一板卡的 `/dev/video25` 连续运行两次非产品原生 V4L2 探针，参数为
PFP、`640x360`、30 帧、`coreMask=0x0`、`MIN_PERSON=1`、`MIN_TRACKING=1`。
该探针使用 V4L2 MMAP 后通过 `VIDIOC_EXPBUF` 将单物理平面 `'NV12'` 的 DMA-BUF fd
交给 RockIVA；它没有声明相机采样率为 10 FPS。原始日志为
`/tmp/t1-v4l2-rockiva-restart-20260831-165409.log`。

| 运行 | 直接退出码 | 序列范围 | captures/pushed/detected/released | person/tracking |
| --- | ---: | --- | --- | --- |
| first | 1 | `48108..48137` | `30/30/30/30` | `0/0` |
| second | 1 | `48156..48185` | `30/30/30/30` | `0/0` |

两次运行的 `sequence_errors` 均为 `0`，DET callback、release callback、
`ROCKIVA_DETECT_Release` 和 `ROCKIVA_Release` 均完成且没有匹配错误；每次停止后
都能再次启动并完成 30 帧，因此只把它记为隔离节点的最小停止/重启边界证据。当前
画面未观察到人员，`MIN_PERSON=1` 和 `MIN_TRACKING=1` 门槛导致两次直接退出码均为
`1`，不能作为有人检测或跟踪证据。

探针前后生产进程的只读快照如下；`media.get_status` 表明当时没有运行中的直播会话，
因此没有形成编码连续性证据。

| 快照 | media_engine PID | RSS | 线程 | 总 FD | `/dev/video24` FD | `media.get_status` |
| --- | ---: | ---: | ---: | ---: | ---: | --- |
| pre | 578 | 68412 kB | 17 | 70 | 2 | `running=false,fps=0,bitrate=0` |
| after first | 578 | 68412 kB | 17 | 70 | 2 | `running=false,fps=0,bitrate=0` |
| post | 578 | 68412 kB | 17 | 70 | 2 | `running=false,fps=0,bitrate=0` |
| post-restore | 578 | 76220 kB | 17 | 70 | 2 | `running=false,fps=0,bitrate=0` |

四个快照中的 `/dev/video24` 名称均为 `rkisp_mainpath`。两次探针结束时按起始格式
恢复了 `/dev/video25` 的多平面 NV12，驱动原始 fourcc 为 `'NM12'`（两物理平面）；由于前一
次 GStreamer 探针曾将该隔离节点留下为低分辨率格式，随后仅对 `/dev/video25` 执行
`v4l2-ctl --set-fmt-video=width=3840,height=2160,pixelformat=NV12,field=none`，
返回码为 `0`，最终格式为 `3840x2160`、单物理平面 `'NV12'`。整个恢复动作没有触碰
`/dev/video24`。

同日单独运行的 GStreamer `rockiva_dmabuf_probe` 在第一个 sample 因
`buffer memory is not DMA-BUF` 被拒绝，`samples_received=1`、`samples_rejected=1`、
`pushed=0`、直接退出码为 `1`；原始日志为
`/tmp/t1-board-dmabuf-20260831-164525.log`。这与上面的原生 `VIDIOC_EXPBUF` 路径
是两个不同的输入实现，不能把 GStreamer 失败写成 RockIVA 检测结果。

## 2026-09-01 原生 V4L2->RockIVA 60 帧有人场景

在设备资源允许的短时窗口内，使用隔离节点 `/dev/video25` 完成一轮 60 帧原生
V4L2 `MMAP + VIDIOC_EXPBUF` 探针。参数为 PFP、`640x360`、`coreMask=0x0`、
`MIN_PERSON=1`、`MIN_TRACKING=1`、`ALLOW_MAINPATH=0`；探针未声明相机采样率。
原始日志为 `/tmp/t1-v4l2-rockiva-person-60-20260901-091122.log`。

| 项目 | 结果 |
| --- | --- |
| sequence | `1807150..1807209`，连续 60 帧 |
| captures/pushed/detected/released | `60/60/60/60` |
| sequence/capture/push/release 错误 | `0/0/0/0` |
| person/tracking observations | `58/50` |
| person states | `FIRST=2`、`TRACKING=50`、`LOST=5`、`DISPEAR=1` |
| detect/release latency | 平均 `18.067/18.106 ms`，范围 `14.954..21.274/14.993..21.305 ms` |
| 直接退出码 | `0`，`ROCKIVA_WaitFinish=-5` 由有界 callback completion fallback 完成收尾 |
| format restore | `ok`，恢复为 `3840x2160 NV12` |
| 日志 SHA-256 | `b45427d67481a996d304591a0834a987f7e2fa5efb6c8ad52e8410befcbc9cd9` |

该轮观察到两个 `objId`：`obj_id=2` 在第 1 帧为 `FIRST`，随后经历
`TRACKING/LOST/DISPEAR`；第 12 帧出现 `obj_id=3` 为 `FIRST`，第 13--60 帧持续
`TRACKING`。这证明了有人场景下单次 DMA-BUF 检出和回调释放闭环，但不证明跨场景
ID 稳定、遮挡恢复、长期启停或生产分支可用性；`obj_id=2` 与 `obj_id=3` 的切换
也不能解释为真实人员进出计数。

本轮结束后的只读收尾快照：`/dev/video25` 无残留进程/fd；`media_engine` PID
578，RSS `68412 kB`，17 线程、70 个 fd，其中 `/dev/video24` 仍为 2 个 fd；
`/dev/video25` 已恢复 `3840x2160` 单物理平面 `NV12`，`/dev/video24` 保持
`3840x2160` 两物理平面 `NM12`。这些快照不替代主路径 sequence、编码连续性和点播
并发验收。

## 2026-09-01 `test1.mp4` CPU-NV12 人群压力样本

为验证用户提供的离线源，使用
`/home/Tiger/Documents/rtsp_demo/test1.mp4`（H.264 Main、`768x432`、约
`59.94 FPS`、`11.878533 s`）生成紧凑 CPU 地址 NV12 输入：

```sh
ffmpeg -ss 1 -i test1.mp4 -vf 'fps=10,scale=640:360:flags=lanczos' \
  -frames:v 60 -pix_fmt nv12 -f rawvideo test1-person-640x360-60.nv12
```

源文件 SHA-256 为
`b3559aebdf182fc0b35ca4009f0ae6ed776b6d577a6695666237ba911affd6d3`；生成的
60 帧输入为 `20,736,000` bytes，SHA-256 为
`54182d45d49875051722e405f20a4332fb4315ae9b7c7a53a9d675af7abb2995`。在板端使用
PFP、`640x360`、60 帧、`FPS=10`、`coreMask=0x0`、`MIN_PERSON=1`、
`MIN_TRACKING=1` 运行 CPU-NV12 探针；原始日志为
`/tmp/test1-board-rockiva-20260901.log`，日志 SHA-256 为
`726d1268777c10606afd062a02870ad4b154c95138c488ddda54297107d6d417`。

| 项目 | 结果 |
| --- | --- |
| push/detect/release callbacks | `60/60/60`，所有 push、检测、释放错误为 0 |
| person observations | `770`（观察次数，不是唯一人数） |
| person states | `FIRST=72`、`TRACKING=425`、`LOST=215`、`DISPEAR=58` |
| 观测到的 `objId` 数量 | `72`（不代表 72 个真实人员） |
| detect/release latency | 平均 `20.483/20.573 ms`，检测范围 `15.229..32.240 ms` |
| SDK 收尾 | `WaitFinish=-5` 走有界 callback fallback，`DETECT_Release`/`Release` 成功 |
| 直接退出码 | `0` |

该样本包含密集人群和遮挡，适合做模型吞吐、目标生命周期和 ID 抖动压力样本；
探针输入仍是 CPU `dataAddr`，不能替代 `/dev/video25` 的 DMA-BUF、实时采集、
主编码并发或精度标注验收。`objId` 数量和 `person observations` 均不得直接转化为
人流唯一计数。

## 2026-09-01 原生 V4L2->RockIVA CLS8 900 帧模型对比

同日继续在隔离节点 `/dev/video25` 上运行 CLS8，作为与 PFP 的模型对比，不属于
产品路径。运行上下文为 `640x360`、V4L2 `MMAP + VIDIOC_EXPBUF`、4 个 buffer、
`channel=0`、`coreMask=0x0`、有限模式 900 帧；探针日志级别为 `events`，因此没有
逐帧时延行。对应环境和入口为：

```sh
MODEL=cls8 DEVICE=/dev/video25 MODEL_PATH=/oem/usr/lib \
ROCKIVA_LIB_DIR=/oem/usr/lib WIDTH=640 HEIGHT=360 FRAMES=900 \
LOG_LEVEL=events REPORT_INTERVAL_MS=1000 MIN_PERSON=1 MIN_TRACKING=1 \
/tmp/run_v4l2_rockiva_probe.sh
```

原始日志为 `/tmp/t1-events-cls8.log`，SHA-256 为
`95317ae72ff0c1a741cb0ff108b4c4c572a4f132462d758ff2610f05c0514f92`。最终摘要和收尾
证据如下：

| 项目 | 结果 |
| --- | --- |
| captures/pushed/detected/released | `900/900/900/900` |
| sequence/capture/push/detection/release 错误 | `53/0/0/0/0`；`qbuf_failures=0` |
| callback completion | `accepted=900`、`detection_completed=900`、`released=900` |
| person/tracking observations | `725/630`（观察次数，不是人数） |
| event `obj_id` 数量 | `15`：`2,3,7,9,10,11,14,18,19,22,23,27,28,30,34` |
| 检测/释放时延 | 本次 `events` 日志未记录逐帧 latency，未据此宣称数值或与 PFP 做时延比较 |
| SDK/采集收尾 | `ROCKIVA_WaitFinish=-5` 走有界 callback fallback；`DETECT_Release=0`、`ROCKIVA_Release=0`、`stream_off=ok`、格式恢复成功 |
| 直接退出码 | 未在本轮 shell 记录中单独捕获；最终摘要为 `t1=not_claimed`，按探针退出判定应视为非零，不能把 `tee/adb` 的传输状态当作探针退出码 |

`sequence_errors=53` 是探针观察到的 V4L2 sequence 不连续次数，不能直接等同于
丢失帧数量；它与采集端在推理未及时消费时发生 overrun/drop 的现象一致。CLS8
本轮推理更重是一个合理的候选原因，但当前 `events` 日志没有采样率、队列深度和
逐帧 latency，尚不能把全部 sequence gap 归因于模型。与已有 PFP 原生 V4L2 有人
场景的 `sequence_errors=0` 证据相比，CLS8 本轮的采集连续性较差。

本轮 15 个 `obj_id` 还伴随频繁的 `FIRST`、`LOST`、`TRACKING`、`DISAPPEAR` 重建；
在现场按单人进出测试理解时，这表现为严重 ID churn 和疑似误检风险。`person=725`
与 `tracking=630` 仍然只是跨帧观察次数，不能作为 15 人、725 人或任何唯一人流量。
因此该结果只能作为 CLS8 对比/候选证据：它证明了 callback 和释放闭环，但不能
解除 T1，也不能支持将 CLS8 固化到生产。PFP 继续保持下一阶段的暂定候选。

## 2026-09-04 GStreamer MP4->RockIVA 人流测试源

为验证离线视频可以通过板端解码器推进 RockIVA DET，停止板端
`media_engine`（PID `574`）后，将 `/home/Tiger/Documents/rtsp_demo/test1.mp4`
（H.264 Main、`768x432`、约 `59.94 FPS`、`11.878533 s`，SHA-256
`b3559aebdf182fc0b35ca4009f0ae6ed776b6d577a6695666237ba911affd6d3`）上传为
`/tmp/me/test1.mp4`。板端实际使用的解码/显示插件为
`libgstrockchipmpp.so`（`mppvideodec`）和 `libgstkms.so`（`kmssink`）。

启用显示分支运行 30 帧：

```sh
SOURCE=mp4 INPUT=/tmp/me/test1.mp4 MODEL_PATH=/oem/usr/lib \
  ROCKIVA_LIB_DIR=/oem/usr/lib WIDTH=640 HEIGHT=640 FPS=10 FRAMES=30 \
  MIN_PERSON=1 MIN_TRACKING=1 LOG_LEVEL=quiet REPORT_INTERVAL_MS=0 \
  DISPLAY_OUTPUT=1 ./run_v4l2_rockiva_probe.sh
```

管线为 `filesrc ! qtdemux ! h264parse ! mppvideodec ! tee`；分析支路复制为
紧凑 CPU `NV12` 后交给 RockIVA，显示支路使用
`rgarotate(rotation=0,out=480x800) ! kmssink(connector=97,plane=75)`。
结果如下：

| 项目 | 结果 |
| --- | --- |
| 直接退出码 | `0` |
| captures/samples/pushed/detected/released | `30/30/30/30/30` |
| capture/push/detection/release 错误 | `0/0/0/0` |
| person/tracking observations | `143/83`（观察次数，不是人数） |
| SDK 收尾 | `WaitFinish=-5` 走有界 callback fallback；`DETECT_Release=0`、`ROCKIVA_Release=0` |
| pipeline | `PLAYING -> NULL`，无 GStreamer 错误消息 |
| 运行日志 SHA-256 | `353cdb9fddbdade9aebaaab407737f716aed63d9ab12ac27150a3a565349b512` |

该结果证明了板端 MP4 解码、显示分支和 CPU-NV12->RockIVA 的一次候选闭环；没有
抓取板端物理屏幕像素，因此“无 GStreamer 错误且完成 KMS 管线”不等同于显示器上
像素结果已独立目视验收。运行期间 MPP 报告了源帧 stride/size mismatch 警告，RGA
报告使用 legacy API；两者未导致本轮退出失败，但需要在进入生产分支前单独评估。
此模式不是生产 DMA-BUF 输入，也不解除 T1 的多轮 DMA-BUF、完整流 epoch、主编码
连续性、点播并发和资源预算门禁。

## 2026-09-04 GStreamer V4L2 DMA-BUF 单内存复测

针对前一轮 GStreamer 探针“单个 memory 但不是 DMA-BUF”的结果，探针补充了
`/dev/dma_heap/system-uncached`（不可用时回退 `/dev/dma_heap/system`）分配器，
并将该分配器同时放入 allocation query 和下游 buffer pool。这样 `v4l2src` 的
多平面采集可以复制到一个带 fd 的下游 DMA-BUF，而不是误把普通系统内存当作
RockIvaImage 的单 `dataFd`。

在 `media_engine` 未运行的状态下，仅使用隔离节点 `/dev/video25`，执行 PFP、
`640x360`、30 帧、`coreMask=0x0`、`MIN_PERSON=0`、`MIN_TRACKING=0` 的生命周期
诊断。生产节点 `/dev/video24` 未作为输入；本次没有停止或启动 `media_engine`。
关键输出为：

```text
appsink allocation=single-memory-dmabuf-copy heap=/dev/dma_heap/system-uncached size=345600
negotiated width=640 height=360 hstride=360 y_stride=640 uv_stride=640
  y_offset=0 uv_offset=230400 logical_planes=2 video_meta=0
  sample_size=345600 max_size=345600 fd=18
summary samples_received=30 samples_rejected=0 pushed=30 push_failures=0
  detection_callbacks=30 detection_errors=0 release_callbacks=30
  released_frames=30 release_unmatched=0 release_duplicates=0
  release_mismatches=0 release_invalid=0 channel_mismatches=0
  detect_latency_ms[min=13.213 max=27.783 avg=16.083]
  release_latency_ms[min=13.291 max=27.894 avg=16.173]
```

探针直接退出码为 `0`；`ROCKIVA_WaitFinish=-5` 由有界 callback completion
fallback 完成，`DETECT_Release=0`、`ROCKIVA_Release=0`。本轮画面没有人员，
因此 `person=0`、`tracking=0` 只说明 30 帧单 fd DMA-BUF 的采集、拷贝、推理和
释放闭环成立，不是检测质量或人流验收。复测结束后只恢复了 `/dev/video25` 为
`3840x2160` 单物理平面 `NV12`；`/dev/video24` 未被重配。

## 当前判定

- PFP 是下一阶段的暂定候选：CPU-NV12 片段、`test1.mp4` 人群压力样本和一轮 60 帧
  有人场景 DMA-BUF 运行均完成回调闭环，且 PFP 的平均回调时延低于 CLS8。样本仍
  不足以形成精度、召回率、ID switch、遮挡恢复或模型发布结论。
- CLS8 的 900 帧原生 V4L2 对比运行完成 `900/900/900/900` callback 闭环，但出现
  `sequence_errors=53` 和 15 个事件 `obj_id`；相较已有 PFP 证据，本轮 CLS8 的采集
  连续性和跟踪稳定性较低。它仍是对比结果，不解除 T1；由于未输出逐帧 latency，
  不据此补写 CLS8 的时延结论。
- `coreMask=0x0` 与 `0x4` 都可工作；当前样本不足以固化核心掩码，生产配置不得
  依据本表硬编码。
- CPU `dataAddr` 的帧所有权、回调关联和 SDK 收尾已在真板得到正向证据。此前
  `WaitFinish` 不支持导致的假失败已由探针能力降级和负向主机测试覆盖。
- 隔离原生探针连续两次停止/重启后，`media_engine` PID 和 `/dev/video24` FD 数量
  未变化；这只说明本次短时复测未观察到主进程退出或主路径 FD 变化，不能替代编码
  连续性、主路径 sequence 或点播回归。
- GStreamer DMA-BUF runner 已能在隔离节点产出单个带 fd 的 DMA-BUF sample，并完成
  30 帧空场景的 RockIVA callback/release 闭环；这解决了探针自身的输入内存类型
  阻塞，但仍需有人场景、多轮重复、完整流 epoch 和生产 GStreamer 分支验证。
  原生 V4L2 有人场景结果和本轮 GStreamer 空场景结果都不能单独解除 T1。

## 未解除的 T1 门禁

1. 多轮有人场景下的 DMA-BUF 检出、`objId/state` 生命周期和稳定性；PFP 当前只有一轮
   原生 V4L2 60 帧候选证据，本轮 GStreamer 30 帧为空场景，CLS8 的 900 帧对比又
   出现 `sequence_errors=53` 和明显 ID 重建，两者都不足以形成 T1 通过证据。
2. 当前仅有两次短时隔离停止/重启的 callback 计数证据；流 epoch 切换、停止时的
   异步回调、无泄漏/UAF 和长期重复启停仍未证明。
3. 与主编码/点播并发时的帧率、丢帧、CPU/NPU、温度和内存预算。
4. 多场景标注集上的检测精度、ID switch、遮挡恢复和方向计数误差；CLS8 本轮的
   15 个 `obj_id`、`person=725` 和 `tracking=630` 只作为抖动/误检风险信号。
5. 已有生产 PID/RSS/FD 的只读前后快照；主路径采集 sequence、编码连续性和点播
   回归仍无可复现证据。

完整的本机原始日志保留在
`/tmp/rockiva-t1-frames.JqRE62/t1-rockiva-*-fallback.log`、
`/tmp/t1-v4l2-rockiva-restart-20260831-165409.log`、
`/tmp/t1-v4l2-rockiva-person-60-20260901-091122.log`、`/tmp/t1-events-cls8.log` 和
`/tmp/t1-board-dmabuf-20260831-164525.log`、`/tmp/test1-board-rockiva-20260901.log`；
它们是会话证据，不作为固件或仓库发布物。
