# 端侧人流检测与 GB28181 告警上送——实施计划

状态：实施中（T1 探针已提供；CPU-NV12、隔离原生 DMA-BUF、一轮 60 帧有人场景及 GStreamer 单内存 DMA-BUF 子门禁证据已有，但因设备资源达到本轮上限暂时暂停更高占用板端复测；T2 已完成）
上游规格：`docs/people-flow-alarm/spec.md`
下游产物：`task.md`、`checklist.md`（另行创建）
本阶段约束：仅在已解除依赖的 T2/T4 主机确定性边界内修改代码；T1 真板结果仍是
进入常驻 RockIVA 分析分支的硬门禁。提交、推送、发布需另行授权。

## 1. 目标

以 `media_engine` + `gb28181_daemon` 两个进程为边界，实现：

- 常驻的人体检测与单路视频内跟踪（首版 RockIVA，`ROCKIVA_MODE_VIDEO`）；
- 事件生命周期 `START / UPDATE / END`、ROI 占用人数与方向性越线增量；
- 有界持久事件日志 + 长连接订阅 + ACK/重放 + 守护进程持久投递 outbox；
- 标准 GB28181 Alarm MESSAGE 上送，兼容 stock WVP；
- 可选 `exact_evidence` 精确证据通道（端侧 JPEG + 独立 HTTP 关联），需 WVP 适配；
- 分析故障不影响主视频流；参数可配置、可回滚。

## 2. 前置决策与默认值

### 2.1 架构边界（已由 spec 固定，不在此重开）

```text
NV12 DMA-BUF → 常驻低分辨率分析分支 → RockIvaBackend
→ NormalizedObservation → EventEngine → EvidenceCache
→ 生产者持久事件日志 → 本地长连接订阅
→ gb28181_daemon DeliveryOutbox → AlarmSink / EvidenceSink
→ SIP Alarm MESSAGE / HTTP 证据 → WVP（stock 或 exact_evidence）
```

RockIVA 只负责检测、跟踪和原始 BA 触发；计数语义、去抖、生命周期、证据和
GB28181 格式化均由业务层负责。

### 2.2 必须由板端 spike 固化的参数

以下参数是候选默认值，不得在未经过真实板端测量的情况下直接写死：

| 参数 | 候选默认 | 固化依据 |
| --- | --- | --- |
| 分析分辨率/帧率 | 640x360@10，或 704x576@10（与模型输入对齐） | 板端 NPU 负载、目标场景最小可检测尺寸 |
| RockIVA 模型 | `ROCKIVA_DET_MODEL_PFP` 或 `CLS8`，仅取 person 类 | 板端可用模型文件、检测精度与帧率 |
| 推理核心 | `coreMask = 0x04` | SDK 样例与板端实测 |
| 结果模式 | BA `detectResultMode = 1`，或纯 DET 全量回调 | 占用人数需要全量目标，不能只收规则触发目标 |
| 分析帧格式 | NV12，`ROCKIVA_IMAGE_FORMAT_YUV420SP_NV12` | 主链原始格式，避免额外转换 |
| 帧输入方式 | DMA-BUF FD + `dataPhyAddr/dataAddr` | 板端内存拷贝成本与 SDK 支持情况 |

### 2.3 业务参数默认值（Phase A 后可调整，但不改契约）

| 配置项 | 默认值 | 说明 |
| --- | --- | --- |
| `analytics.enabled` | `false`（出厂关） | 关闭时保持现有行为，作为回滚开关 |
| 事件确认 | 连续 3 帧或 0.3s | 防单帧误检 |
| 消失宽限 | 1.0s | 短遮挡不立即结束 |
| 冷却 | 5.0s | 防止同一轨迹反复触发 |
| `UPDATE` | 默认不上送 | 只记录本地，可按平台开启并限频（如 5s） |
| `END` | stock WVP 不上送 | 适配平台明确支持时按配置开启 |
| Alarm 映射 | Method=5；越线 Type=5、入侵 Type=6、流量统计 Type=9 | 可配置 |
| AlarmPriority | 1（可配置） | 按平台习惯与 GB28181 范围校验 |
| 证据模式 | `stock_wvp_pull`（默认） | `exact_evidence` 为可选适配模式 |
| 证据 JPEG | MPP `mppjpegenc` 优先，`ROCKIVA_IMAGE_Write` 兜底 | 以板端时延/质量实测为准 |
| 证据保留 | 512 MB 或 72h，先到先删 | 待板端存储实测调整 |
| 事件日志 | 4096 条或 64 MB，`START/END` 保护 | 队满先合并/丢弃 `UPDATE` |
| 重试 | 5s 起步，指数退避至 5min，带抖动 | SIP/HTTP 与本地桥统一策略 |
| 时钟 | PTS 为主时间线，`AlarmTime` 由采集时刻映射 | NTP 跳变不得倒序 |

## 3. 阶段划分与门禁

### Phase A：板端 RockIVA 验证（先决门禁）

目的：在投入生产代码前，验证 RockIVA 在该板上能稳定检测/跟踪，并固化模型、
分辨率、帧率、核心掩码、回调与内存语义。

产出：

- `tests/board/rockiva_probe`（非产品代码）：NV12 测试图/相机帧 → RockIVA
  init → push → release callback → DET 回调打印；BA 规则验证在检测输入和
  生命周期确认后单独进行，避免把规则触发结果误当成全量目标结果；
- 板端运行脚本与基准记录（帧率、时延、内存、NPU 占用）；
- 决策记录：模型选择、分析分辨率/帧率、`detectResultMode`、BA 规则或纯 DET；
- 确认 DMA-BUF FD 生命周期与 `ROCKIVA_FrameReleaseCallback` 的时序。

当前已提供 `media_engine/tests/board/rockiva_probe/`：它通过 SDK 交叉工具链
构建 AArch64 探针，使用紧凑 CPU 地址 NV12 文件验证 DET 回调、对象
`objId/state/score/rect/frameId`、释放回调和逐帧时延。该工具仍不是产品目标；
2026-08-31 已在隔离 `/dev/video25` 上补充原生 V4L2 MMAP/EXPBUF 到 RockIVA 的
DMA-BUF 生命周期验证；生产 GStreamer 分支、多人/多轮有人场景下的 DMA-BUF
检出/tracking、物理地址输入和参数固化仍待真板证据。CPU-NV12 和单轮 DMA-BUF
有人片段证据不替代这些门禁。

2026-09-04 又在同一隔离节点完成 GStreamer `v4l2src ! appsink` 的单内存
DMA-BUF 拷贝试验：探针专用 dma-heap 分配器产出单个带 fd 的 `640x360` sample，
30 帧空场景均完成 RockIVA push、DET callback、release callback 和 SDK 收尾。
这解决了测试 runner 的输入内存类型阻塞，但没有证明有人场景、多轮完整流 epoch、
主编码并发或生产分支可用；`person/tracking=0` 也不构成检测证据。

2026-08-27 的连接阻塞及 2026-08-31 设备资源上限暂停记录保留在
`docs/people-flow-alarm/t1-board-blocker.md`。CPU-NV12 已取得有人场景检测/跟踪
数据，另用 `test1.mp4` 完成密集人群 CPU-NV12 压力样本；隔离 `/dev/video25` 已取得
原生 DMA-BUF 生命周期和短时停止/重启数据。当前暂停不是检测失败，而是设备资源达到
本轮上限。2026-09-01 已取得一轮有人场景 DMA-BUF `person=58 tracking=50` 候选结果，
以及 CPU 样本 `person=770 tracking=425`；完整流 epoch、多轮稳定性、主编码连续性和
点播并发仍未复核。T1 完成前不推进
B2/B3 生产分析分支；资源恢复后的最小入口固定使用 `/dev/video25`，不得操作
`/dev/video24`。

验收门禁：

- 目标场景 person 检出稳定，`objId/state` 生命周期符合预期；
- release callback 与 push 不泄漏、不重复释放；
- 640x360~896x512 范围内推理帧率 ≥ 配置采样率；
- 关掉点播时分析分支持续运行；
- 主视频流不受分析启停影响。

### Phase B：media_engine 分析、事件与本地桥

#### B1 配置扩展

- `EngineConfig` 增加分析开关、模型、分辨率/帧率、阈值、规则几何、证据目录、
  日志容量、时钟策略；
- YAML/命令行解析与校验，非法值启动即报错；
- 新增配置样例与单元测试。

#### B2 常驻分析分支

- 基础管线 tee 增加分析分支：`queue leaky=downstream → rga 缩放 → appsink`；
- 分支不依赖 INVITE/点播，采集启动即建立；
- 有界最新帧队列，推理落后时丢旧帧；
- 分支错误与主链路隔离，失败时只降级分析健康状态。

#### B3 RockIvaBackend

- 独立模块封装 RockIVA 生命周期、push、release 回调；
- 输出 `NormalizedObservation`：`channel_id`、`stream_epoch`、`frame_id`、
  `source_pts`、`capture_time/clock_state`、分析帧尺寸、后端版本、person
  `track_id/bbox/class/score/state`、可选 BA `rule_id/rule_type/direction`；
- 坐标统一为分析帧万分比或带尺寸的归一化坐标，禁止直接泄漏厂商结构；
- `stream_epoch` 在进程启动/流重置时递增，跟踪 ID 作用域为
  `(channel_id, stream_epoch, obj_id)`。

#### B4 EventEngine

- 规则评估、确认、去抖、宽限、冷却、恢复；
- `START/UPDATE/END` 与 `event_id/event_seq/reason/event_time/source_pts/
  frame_id/person_count/delta_in/delta_out`；
- 流重置、重配置、分析关闭、进程重启的结束原因；
- 重复/乱序帧不得使状态倒退；
- 规则配置带版本，变更时先收尾旧事件。

当前已完成主机确定性实现：`event_engine.{h,c}` 以固定容量状态保存单路事件，
使用 PTS 优先的单调内部时间线，保留采集墙上时间的缺失状态，并通过同步回调
输出完整 `START/UPDATE/END`。越线规则事实可带责任 `track_id`，同一事件会聚合
方向增量和责任轨迹；ROI 边界切换使用规则去抖时间确认。该实现尚未连接 RockIVA、
证据缓存或 daemon，观察结果契约已递增到版本 2。

#### B5 EvidenceCache

- 触发帧选择：精确帧优先，最近帧降级必须标记近似并记录帧差；
- JPEG 编码（MPP 优先）、原子写入、校验和、元数据记录；
- 容量与保留策略：`evidence_id/event_id/frame_id/source_pts` 关联；
- stock 模式不产生图片；exact 模式图片不随 Alarm XML 发送。

#### B6 生产者事件日志与订阅桥

- 有界持久日志（追加 JSONL + 校验 + fsync + 原子压缩），`delivery_cursor`
  单调递增；
- 新增长连接订阅协议：`subscribe / ack / resume`，不复用短请求响应；
- 断线重放未确认记录；队满保护 `START/END`；
- 协议版本号与兼容策略；日志损坏有明确恢复路径与诊断计数。

#### B7 验证

- 宿主单测：事件状态机、计数、时间、日志/重放/队满、证据选择；
- 板端联调：分析分支常驻、RGA 缩放、回调转换、日志落盘、断连重放。

门禁：

- `make -C tests` 与板端脚本通过；
- 事件契约、日志游标、证据元数据符合 spec；
- 分析关闭时行为与当前基线一致。

### Phase C：gb28181_daemon 告警与投递

#### C1 订阅客户端

- 长连接订阅 media 事件日志，持久化最后确认游标；
- 守护进程重启后 resume，未确认记录重放；
- 本地桥错误进入健康指标，不阻塞 SIP 注册/点播。

#### C2 DeliveryOutbox

- 守护进程侧持久 outbox（追加 JSONL + fsync + 原子压缩）；
- 每个记录分别维护 Alarm 与 Evidence 两个 sink 状态；
- durable ingest 前崩溃由生产者重放，之后崩溃由 outbox 恢复；
- 图片内容在确认前复制/硬链接进 outbox 或取得持久租约；
- 幂等键：设备/通道、`event_id`、阶段、事件内序号；
- 成功 sink 不因另一 sink 失败重发；重试耗尽进入失败状态并保持可观测。

#### C3 AlarmSink

- `gbxml.Alarm` 构建：`CmdType=Alarm`、`DeviceID`、`AlarmPriority`、
  `AlarmMethod`、`AlarmTime`、`AlarmDescription`、可配置 `AlarmType`；
- `SN` 独立分配与回绕处理；
- 默认 `START`；`UPDATE` 默认关；stock WVP 下 `END` 默认关；
- SIP 响应/超时/注册状态进入重试决策；本地写入成功不等于平台投递成功；
- 黄金 XML 与解析器级测试：转义、必填字段、时区、Unicode、无图片字节。

#### C4 EvidenceSink

- `exact_evidence` 模式：HTTP 上传或平台拉取适配器；
- 认证、校验和、幂等、独立重试、与 Alarm 到达顺序解耦；
- 证据失败不阻塞 Alarm sink。

门禁：

- `make test`、`make vet` 通过；
- outbox 崩溃恢复/重放/幂等测试通过；
- WVP 能收到并列出标准 Alarm。

### Phase D：WVP 集成

- `stock_wvp_pull`：在本地 WVP 2.7.4 + ZLM 环境验证 Alarm 入库，确认其
  异步抓图是“附近帧”而非触发帧，文档化限制；
- `exact_evidence`：新增 WVP 侧小适配器，按 `event_id/evidence_id` 幂等关联；
  图片可先于或晚于告警到达；不在标准 Alarm XML 内塞图片；
- 验证 Alarm 列表、图片展示、重复投递去重、降级策略。

### Phase E：整板验收与发布

验收项：

- 目标场景精度/召回率、ID switch、方向计数误差；
- 触发到 Alarm 时延、触发到证据对齐；
- NPU/CPU/RGA 占用、内存增长、温度、分析丢帧；
- 编码连续性、点播不受分析影响；
- 队列背压、SIP/HTTP 断线重放、重启恢复；
- 24h 稳定性（正常流量 + 计划内上游中断）；
- 固件布局与自启：RockIVA 库/模型入 `/oem`，配置入 `/oem` 与 `/userdata`。

发布前必须记录：

- 验收阈值（精度、计数误差、时延、资源上限）；
- 测试板、固件版本、模型版本、配置快照；
- 已知限制（stock WVP 抓图非精确、ID 复用、队列丢弃等）。

## 4. 依赖

| 依赖 | 状态 | 说明 |
| --- | --- | --- |
| RockIVA 头文件与 `librockiva.so` | 已在 `output/out/media_out` | 需要确认固件打包与 license |
| RockIVA 模型文件 | 待确认 | `/oem/usr/lib` 下模型与 `coreMask` 对应 |
| MPP JPEG 编码 | 已见 `mppjpegenc` | 板端时延/质量实测 |
| 现有 `media_engine` 管线 | 已落地 | 常驻采集、tee、RGA、IPC |
| 现有 `gb28181_daemon` | 已落地 | SIP 注册/心跳/会话；Alarm 待补 |
| WVP 2.7.4 + ZLM | 已用于联调 | exact 模式需单独适配器 |

## 5. 风险与缓解

| 风险 | 影响 | 缓解 |
| --- | --- | --- |
| RockIVA 模型/许可证不可用 | 无法首版落地 | Phase A 先验证；失败则回退 RKNN 扩展计划 |
| DMA-BUF 生命周期/release 回调时序 | 内存泄漏或 UAF | Phase A spike；板端 gdb/asan 验证 |
| NPU 推理慢于采集 | 分析丢帧、占用人数滞后 | 低分辨率 + 采样率 + 最新帧策略；可观测丢帧率 |
| 事件日志/outbox 写入闪存 | 磨损、写入延迟 | 有界、批量 fsync、压缩、容量监控 |
| stock WVP 对 `END/UPDATE` 语义 | 告警列表出现重复/误读 | 默认只上送 `START`，`END` 按平台配置 |
| WVP 异步抓图非触发帧 | 复核时误导 | 文档化；exact 模式独立通道 |
| PTS/墙上时间映射误差 | AlarmTime 不准 | 纪元级映射、时钟有效状态、误差可观测 |
| 队列背压 | 丢事件 | 先合并/丢 `UPDATE`，保护 `START/END`，计数可见 |

## 6. 回滚与兼容

- 出厂默认 `analytics.enabled=false`，关闭时保持现有采集/编码/点播行为；
- 新 IPC 协议带版本号；旧 daemon 与 media_engine 混合启动时明确报错或按
  兼容模式运行；
- stock WVP 路径只依赖标准 Alarm 字段，可在无任何 WVP 适配时运行；
- 若板端验证未达阈值，可在 Phase A 门禁停止，不影响主视频功能；
- 每次固件升级保留上一版镜像；配置样例与固件布局同步发布。

## 7. 不在本计划内（同 spec Out of Scope）

- 自训练/量化/调优 RKNN 模型；
- `wvp-ssv` 主机推理栈移植；
- BoT-SORT（除非真板证明 RockIVA ID 不达标）；
- 跨摄像机 ReID/全局唯一访客；
- Alarm XML 内嵌图片；
- WVP UI/媒体服务器全面重做。

## 8. 下一步

1. 设备资源恢复后，在真实 RV1126B 的隔离 `/dev/video25` 上重复有人场景 T1 RockIVA
   probe，再补齐完整流 epoch、主编码连续性、点播并发和资源预算证据；
2. 在不等待 T1 的前提下完成 T4 事件引擎主机确定性实现与测试；
3. T1/T4 均通过后再进入 T3 常驻分析分支，并继续 T5/T6 的事件桥与 Alarm 投递。
