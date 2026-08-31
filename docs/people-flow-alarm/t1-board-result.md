# T1 RV1126B RockIVA 探针结果

日期：2026-08-28、2026-08-31（Asia/Shanghai）

## 范围与输入

本记录只覆盖非产品探针的 CPU 地址 NV12 输入和 RockIVA DET 回调；未改动
`media_engine`、daemon 或板端持久配置。输入来自已目视确认包含施工人员的
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

## 当前判定

- PFP 是下一阶段的暂定候选：在这一个未标注、3 秒的人员片段上，三组均稳定完成，
  且 PFP 的平均回调时延低于 CLS8。该结果不构成精度、召回率、ID switch 或模型
  发布结论。
- `coreMask=0x0` 与 `0x4` 都可工作；当前样本不足以固化核心掩码，生产配置不得
  依据本表硬编码。
- CPU `dataAddr` 的帧所有权、回调关联和 SDK 收尾已在真板得到正向证据。此前
  `WaitFinish` 不支持导致的假失败已由探针能力降级和负向主机测试覆盖。
- 隔离原生探针连续两次停止/重启后，`media_engine` PID 和 `/dev/video24` FD 数量
  未变化；这只说明本次短时复测未观察到主进程退出或主路径 FD 变化，不能替代编码
  连续性、主路径 sequence 或点播回归。
- GStreamer DMA-BUF runner 尚未产出可提交给 RockIVA 的 DMA-BUF sample；需要先解决
  输入内存类型协商，再单独验证该链路。

## 未解除的 T1 门禁

1. 有人场景下的 DMA-BUF 检出、`objId/state` 生命周期和稳定性。
2. 当前仅有两次短时隔离停止/重启的 callback 计数证据；流 epoch 切换、停止时的
   异步回调、无泄漏/UAF 和长期重复启停仍未证明。
3. 与主编码/点播并发时的帧率、丢帧、CPU/NPU、温度和内存预算。
4. 多场景标注集上的检测精度、ID switch、遮挡恢复和方向计数误差。
5. 已有生产 PID/RSS/FD 的只读前后快照；主路径采集 sequence、编码连续性和点播
   回归仍无可复现证据。

完整的本机原始日志保留在
`/tmp/rockiva-t1-frames.JqRE62/t1-rockiva-*-fallback.log`、
`/tmp/t1-v4l2-rockiva-restart-20260831-165409.log` 和
`/tmp/t1-board-dmabuf-20260831-164525.log`；它们是会话证据，不作为固件或仓库发布物。
