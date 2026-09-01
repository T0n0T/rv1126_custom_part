# 端侧人流检测与 GB28181 告警上送任务分解

状态：T0/T2/T4 已完成；T1 已取得 CPU-NV12、隔离采集 DMA-BUF 生命周期、一轮 60 帧有人场景及一轮 CLS8 900 帧对比子证据，但 CLS8 对比出现 `sequence_errors=53` 和明显 ID 重建，且因设备资源达到本轮上限而暂停；T3 仍阻塞于 T1
上游规格：`docs/people-flow-alarm/spec.md`
上游计划：`docs/people-flow-alarm/plan.md`
本文件作用：记录经确认的 T0-T12 纵向任务、阻塞关系和验收边界

## 任务总览

每项任务都应形成一条可独立验证的最小闭环。被阻塞的任务不得提前进入生产实现；只有 T0 和满足其依赖的任务可以进入当前实施前沿。

| 编号 | 任务 | Blocked by | Status |
| --- | --- | --- | --- |
| T0 | 基线与回滚开关 | 无 | completed |
| T1 | 真板 RockIVA 探针 | T0 | in progress |
| T2 | 观察结果与配置契约 | T0 | completed |
| T3 | 常驻分析分支 | T1、T2 | blocked |
| T4 | 事件引擎 | T2 | completed |
| T5 | 证据缓存与持久事件桥 | T3、T4 | blocked |
| T6 | daemon Outbox 与标准 Alarm | T5 | blocked |
| T7 | 精确证据 Sink | T5、T6 | blocked |
| T8 | stock WVP 兼容验证 | T6 | blocked |
| T9 | 可观测性、故障隔离与打包 | T3、T5、T6 | blocked |
| T10 | exact-evidence WVP 适配器 | T6、T7 | blocked |
| T11 | stock WVP 真板发布验收 | T1、T3、T4、T5、T6、T8、T9 | blocked |
| T12 | exact-evidence 发布验收 | T10、T11 | blocked |

依赖图：

```text
T0 -> T1
T0 -> T2
T1 + T2 -> T3
T2 -> T4
T3 + T4 -> T5
T5 -> T6
T5 + T6 -> T7
T6 -> T8
T3 + T5 + T6 -> T9
T6 + T7 -> T10
T1 + T3 + T4 + T5 + T6 + T8 + T9 -> T11
T10 + T11 -> T12
```

## 任务验收边界

### T0 — 基线与回滚开关

**What to build:** 建立现有媒体、daemon、固件和 WVP 互操作基线，并让分析功能具备明确的关闭路径。

**Blocked by:** None — can start immediately.

**Status:** completed

- [x] 记录当前视频采集、编码、点播、GB28181 注册和 INVITE/BYE 基线。
- [x] 验证分析关闭时现有视频行为保持不变。
- [x] 记录基线固件、配置、工具链和测试命令，作为后续回归对照。

详见 `docs/people-flow-alarm/baseline.md`。真板、交叉编译和 WVP 运行态项目明确保留给后续门禁。

### T1 — 真板 RockIVA 探针

**What to build:** 在真实 RV1126B 上运行一个不属于产品路径的 RockIVA 探针，确认人体检测、视频跟踪和帧所有权可以稳定工作。

**Blocked by:** T0 — 基线与回滚开关。

**Status:** in progress（CPU-NV12 有人片段、`test1.mp4` 人群压力样本和一轮 60 帧有人场景 DMA-BUF 候选运行通过；隔离原生 DMA-BUF 生命周期和短时停止/重启边界通过；CLS8 900 帧模型对比虽完成 callback 闭环但出现 `sequence_errors=53` 和明显 ID 重建；因设备资源达到本轮上限暂时停止更高占用板端复测；完整流边界和主视频连续性仍待验证）

探针已落地为非产品路径：`media_engine/tests/board/rockiva_probe/`。它提供
SDK 交叉编译 Makefile、运行脚本、CPU 地址 NV12 输入、DET 回调、帧释放回调、
对象字段和逐帧时延统计；默认成功门槛还要求出现 person 和 `TRACKING` 状态。
该门槛不替代同一 `objId` 生命周期、ID switch 和遮挡恢复质量的真板验收。
已复制到真实 RV1126B 并完成 CPU-NV12 专项试验，以及 `/dev/video25` 隔离节点的
原生 V4L2 MMAP/EXPBUF 到 RockIVA 生命周期试验。2026-08-31 连续两次各采集 30 帧，
序列分别为 `48108..48137`、`48156..48185`，每次均为 `30/30/30/30` capture/push/
detect/release，`sequence_errors=0`；两次直接退出码均为 `1`，原因是当前画面没有
person/tracking。该结果只支持短时隔离停止/重启边界，不替代有人场景的多轮稳定性、
完整流 epoch、异步停止回调或主视频连续性结论，因此本任务尚未完成。

2026-09-01 在同一 `/dev/video25` 上完成一轮 60 帧有人场景运行：序列
`1807150..1807209` 连续，`captures/pushed/detected/released=60/60/60/60`，所有
sequence/capture/push/release 错误计数为 0，`person=58`、`tracking=50`，观察到
`obj_id=2` 的 `FIRST/LOST/TRACKING/DISPEAR` 与 `obj_id=3` 的 `FIRST/TRACKING`。
直接退出码为 0；`ROCKIVA_WaitFinish=-5` 由有界 callback completion fallback
完成收尾。该轮只是单次候选证据，仍需多场景、重复运行和长期边界验证。

同日使用 `/home/Tiger/Documents/rtsp_demo/test1.mp4` 生成 60 帧 `640x360 NV12`
CPU 地址压力样本，PFP 探针结果为 `pushed=60`、`detection_callbacks=60`、
`released_frames=60`、`person=770`、`tracking=425`，直接退出码为 0。该样本用于
密集人群压力观察，不替代 DMA-BUF 或唯一人数验收。

同日另在 `/dev/video25` 使用 CLS8、`640x360`、`coreMask=0x0`、4 个 V4L2 buffer
和有限模式 900 帧进行原生 `MMAP + VIDIOC_EXPBUF` 模型对比。运行结果为
`captures/pushed/detected/released=900/900/900/900`，`capture/push/detection/release`
错误均为 0，但 `sequence_errors=53`；`person=725`、`tracking=630`，观察到 15 个
事件 `obj_id`。`ROCKIVA_WaitFinish=-5` 由有界 callback fallback 收尾，
`DETECT_Release`/`ROCKIVA_Release` 成功；最终摘要为 `t1=not_claimed`，本轮没有
单独捕获探针直接退出码，不能把 `tee/adb` 的传输状态当作探针退出码。本次 `LOG_LEVEL=events` 未记录逐帧 latency，因此不补写 CLS8
时延结论。

这只是模型比较结果，不是 T1 完成证据。`sequence_errors=53` 表明 V4L2 sequence
连续性未通过，不能直接等同于 53 个丢帧；推理较重导致采集 overrun/drop 是合理但
尚未证实的原因。15 个 `obj_id` 及频繁 `FIRST/LOST/TRACKING/DISAPPEAR` 重建，
在单人现场语境下构成严重 ID churn/疑似误检风险。`person` 和 `tracking` 是跨帧
observation 次数，不是人数；相较已有 PFP `sequence_errors=0` 的有人场景候选证据，
CLS8 本轮观察到的吞吐连续性和跟踪稳定性较低，PFP 仍为暂定候选。

当前暂停不是检测失败。CPU-NV12 已有包含人员的正向检测/跟踪证据，隔离
`/dev/video25` 已有原生 DMA-BUF capture/push/detect/release、短时停止/重启以及一轮
60 帧有人场景候选证据；但设备资源已达到本轮上限，现阶段不再执行更高占用板端命令。
恢复后仍需补多轮有人场景 DMA-BUF 检出/tracking、完整流 epoch（包括异步停止回调和
长期重复启停）以及主编码连续性/点播并发证据。最小恢复入口见 `t1-board-blocker.md`，固定使用
`/dev/video25`，不得触碰 `/dev/video24`。

2026-08-27 的网络不可达记录保留在 `docs/people-flow-alarm/t1-board-blocker.md`；
2026-08-28 已通过 ADB 在同一板卡完成 CPU-NV12 PFP/CLS8 探针。结果、库/模型指纹、
`ROCKIVA_WaitFinish` 不支持的受限 fallback，以及未完成边界见
`docs/people-flow-alarm/t1-board-result.md`。这些证据不解除 T3：有人场景下的
DMA-BUF 已有一轮 60 帧检出/tracking 候选证据；短时隔离停止/重启已有证据，但完整流 epoch、异步回调、
UAF/泄漏、主路径 sequence 和编码连续性仍未验证。另一次 GStreamer DMA-BUF runner
在第一个 sample 因内存不是 DMA-BUF 被拒绝，不能与原生 `VIDIOC_EXPBUF` 结果混同。

- [ ] 在候选分辨率和采样率下取得稳定 DMA-BUF person 检出及 `objId/state` 生命周期；
      2026-09-01 原生 V4L2 60 帧 PFP 运行已有 `person=58 tracking=50` 的候选证据，
      但 CLS8 900 帧对比出现 `sequence_errors=53`、15 个 `obj_id` 和频繁 ID 重建，
      两者仍需多轮场景和长期稳定性验证。
- [ ] 固化经过测量的模型、结果模式、核心掩码、帧格式和输入方式。
- [ ] 证明 push、release callback、停止和重启无泄漏、重复释放或悬空引用。
- [ ] 证明分析分支关闭点播后仍可运行，且启停不影响主视频。

### T2 — 观察结果与配置契约

**What to build:** 提供后端无关、可版本化的观察结果和分析配置，使没有 NPU 时也能确定性驱动后续事件链路。

**Blocked by:** T0 — 基线与回滚开关。

**Status:** completed

- [x] 定义通道、流纪元、帧 ID、PTS、采集时间、时钟状态、尺寸、后端版本和人体轨迹字段。
- [x] 定义分析开关、规则几何、确认/去抖/宽限/冷却、证据和日志限制的校验规则。
- [x] 为缺失时间、重复帧、乱序帧和配置版本变化定义可观察行为。
- [x] 提供固定时间的合成观察结果夹具，能被不依赖 NPU/墙钟的主机测试复用。

实现证据：`media_engine/src/analytics/observation.{h,c}`、
`media_engine/src/analytics/config.{h,c}`、`media_engine/tests/observation_test.c` 和
`media_engine/tests/config_test.c`。配置默认关闭分析，启用时要求待真板确认的模型、
偶数尺寸和采样率；本阶段没有固化 RockIVA 模型、核心掩码或输入内存参数。

### T3 — 常驻分析分支

**What to build:** 让基础媒体管线在不依赖点播的情况下持续产生归一化观察结果，并在推理异常时保持视频可用。

**Blocked by:** T1 — 真板 RockIVA 探针；T2 — 观察结果与配置契约。

**Status:** blocked

- [ ] 采集启动时建立有界、最新帧优先的低分辨率分析分支。
- [ ] 分析帧提交、丢帧、缩放和释放不阻塞编码/采集主路径。
- [ ] RockIVA 回调输出符合 T2 契约，轨迹身份限定在通道和流纪元内。
- [ ] RockIVA 故障、超时或队列满只造成分析降级并产生指标。

### T4 — 事件引擎

**What to build:** 将归一化观察结果转换为稳定的人流事件生命周期，区分 ROI 当前人数和方向性流量。

**Blocked by:** T2 — 观察结果与配置契约。

**Status:** completed

- [x] 对确认、误检、遮挡、边界抖动、冷却和规则重配置产生确定性结果。
- [x] 生成稳定的 `event_id`、`START/UPDATE/END`、原因、计数、方向增量、帧 ID 和 PTS。
- [x] 流重置、分析关闭、进程重启和乱序/重复帧不会使状态倒退。
- [x] 不把跟踪 ID 包装成跨摄像机或全局唯一访客数。

实现证据：`media_engine/src/analytics/event_engine.{h,c}` 和
`media_engine/tests/event_engine_test.c`。引擎只消费 T2 归一化观察结果；本地事件
始终产生完整生命周期，`send_updates/send_end` 留给下游 Alarm sink 决定。
测试覆盖占用 ROI、越线方向、确认、去抖、消失宽限、冷却、重复/乱序帧、流纪元切换、
进程重启收尾和配置重置，并验证越线责任轨迹和 ROI 边界迟滞。观察结果契约版本
从 1 递增到 2 以承载可选责任 `track_id`。尚未接入 RockIVA、证据缓存或 daemon 投递。

### T5 — 证据缓存与持久事件桥

**What to build:** 将事件及其精确或近似证据写入可恢复的本地链路，支持长连接订阅、确认和断线重放。

**Blocked by:** T3 — 常驻分析分支；T4 — 事件引擎。

**Status:** blocked

- [ ] 触发帧优先选择精确证据，降级到近邻帧时记录差异并标记近似。
- [ ] 图片发布原子化，证据元数据包含 `event_id`、`evidence_id`、帧 ID、PTS、校验和及保留状态。
- [ ] 事件先写入有界持久日志，再通过带游标的长连接发送。
- [ ] ACK、resume、队满合并/丢弃和损坏恢复行为可由主机测试验证。

### T6 — daemon Outbox 与标准 Alarm

**What to build:** 让 GB28181 daemon 能可靠接收事件、持久化投递状态并向上级平台发送标准 Alarm MESSAGE。

**Blocked by:** T5 — 证据缓存与持久事件桥。

**Status:** blocked

- [ ] daemon 重启后可从最后确认游标继续接收，重复事件按幂等键处理。
- [ ] Outbox 分别记录 Alarm 和证据 sink 状态，成功 sink 不因另一 sink 失败而重发。
- [ ] 标准 Alarm XML 包含必需字段，`SN`、事件序号、帧 ID 和 PTS 严格分离。
- [ ] Alarm XML 不嵌入 JPEG、Base64 或端侧本地路径；默认只投递 START。
- [ ] SIP 超时、拒绝、未注册和重试耗尽均可观测且不影响媒体主链路。

### T7 — 精确证据 Sink

**What to build:** 在启用 `exact_evidence` 时，通过独立 HTTP 通道传输可校验、可重试、可幂等关联的 JPEG 证据。

**Blocked by:** T5 — 证据缓存与持久事件桥；T6 — daemon Outbox 与标准 Alarm。

**Status:** blocked

- [ ] 证据内容在确认前具有持久副本或有效保留租约。
- [ ] HTTP 认证、校验和、幂等键、重试退避和永久失败状态可测试。
- [ ] 图片和 Alarm 可任意顺序到达，证据失败不阻塞 Alarm 投递。
- [ ] 默认 stock 模式不启用该 Sink，也不改变标准 Alarm 契约。

### T8 — stock WVP 兼容验证

**What to build:** 在不修改 WVP 解析器的情况下接收标准 Alarm，并明确平台异步抓图只是告警附近帧。

**Blocked by:** T6 — daemon Outbox 与标准 Alarm。

**Status:** blocked

- [ ] stock WVP 接受并保存标准 Alarm，字段映射和类型符合目标版本行为。
- [ ] 重复投递不会造成不可控的消息风暴，SIP 重试结果可追踪。
- [ ] WVP 生成的图片与端侧触发帧差异被记录为兼容模式限制。

### T9 — 可观测性、故障隔离与打包

**What to build:** 让分析事件链路具备生产可诊断性、资源边界和可回滚的固件交付形态。

**Blocked by:** T3、T5、T6。

**Status:** blocked

- [ ] 暴露推理、跟踪、事件、证据、日志、订阅、SIP、时钟和降级指标。
- [ ] 注入 NPU、存储、本地 socket、SIP 和 HTTP 故障时主视频持续工作。
- [ ] 配置、模型、RockIVA 库和自启路径可检查，容量和保留上限有界。
- [ ] 提供回滚到现有视频基线的操作和诊断说明。

### T10 — exact-evidence WVP 适配器

**What to build:** 为已适配的 WVP 提供独立证据接收和告警关联，使图片先到或后到都能正确归档。

**Blocked by:** T6、T7。

**Status:** blocked

- [ ] 通过 `event_id/evidence_id` 幂等关联标准 Alarm 和 HTTP 证据。
- [ ] 支持图片先到、Alarm 先到、重复请求和重启恢复。
- [ ] 关联失败、校验失败和过期证据具有明确的可见状态。
- [ ] 不要求在 GB28181 Alarm XML 中加入非标准图片字段。

### T11 — stock WVP 真板发布验收

**What to build:** 在代表性真实场景完成 stock WVP 路径的整板发布判定。

**Blocked by:** T1、T3、T4、T5、T6、T8、T9。

**Status:** blocked

- [ ] 记录标注场景上的检测精度、ID switch、方向计数误差和端到端时延。
- [ ] 记录 NPU/CPU/RGA、内存、温度、分析丢帧、编码连续性和队列压力。
- [ ] 验证断线重放、daemon/媒体重启恢复和至少 24 小时稳定性。
- [ ] 形成包含固件、模型、配置、阈值和已知限制的发布记录。

### T12 — exact-evidence 发布验收

**What to build:** 在 stock 基线验收通过后，完成精确证据链路的部署级验收和降级判定。

**Blocked by:** T10、T11。

**Status:** blocked

- [ ] 验证触发帧到证据内容的帧 ID、PTS、时间和校验关联。
- [ ] 验证 HTTP 断线、重复请求、到达乱序、保留上限和失败重试。
- [ ] 证明 exact 失败时可按策略降级为 Alarm-only 或 stock 拉图，并明确标记。
- [ ] 形成 exact-evidence 版本的发布、回滚和运维说明。

## 实施门禁

1. 初始提交可以纳入现有 media/daemon 基线源码，但不得包含改变生产行为的分析代码或未经真板验证的 RockIVA 参数结论；规格、计划、任务票据和验收清单必须同步保存。
2. T0 完成后才允许进行 T1/T2；T1 的真实板结果是进入生产分析分支的硬门禁。
3. 未完成 T6 和 T8 前，不宣称 stock WVP 告警链路可用。
4. 未完成 T10 和 T12 前，不宣称精确证据链路可用。
5. 每个任务完成后更新对应票据状态和 `checklist.md`，提交/推送仍需单独授权。
