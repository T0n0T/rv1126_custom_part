# WVP 人流精确证据适配器契约

状态：WVP 侧适配器代码和本地测试已完成；WVP/ZLM/SIP 运行态联调未开始。

本文只定义 `exact_evidence` 与 WVP 的适配边界，不改变 stock WVP 的
GB28181 Alarm 解析、入库和抓图行为。

## 联调目的

WVP 联调分为两个互不替代的验收面：

1. `stock_wvp_pull` 验证标准 Alarm 能被 WVP 接收、保存和展示，并记录 WVP
   异步抓图是告警附近帧，不是 RockIVA 触发帧。
2. `exact_evidence` 验证端侧 JPEG 能按 `event_id/evidence_id` 与标准 Alarm
   独立关联，且支持图片先到、Alarm 先到、重复投递、重启恢复和失败可见。

MP4 解码、RockIVA 推理、屏幕显示和 `/dev/video25` 的板端门禁不属于 WVP
联调目的。

## 请求契约

daemon 使用配置项 `events.evidence_url` 发送一个 `POST` multipart 请求。
当前 WVP 适配器使用独立路径 `/api/v1/people-flow/evidence`；反向代理可以
在部署时改变外部路径，但 WVP 内部入口和接口鉴权例外必须保持一致。

适配器默认关闭。启用时至少配置：

```yaml
people-flow:
  exact-evidence:
    enabled: true
    bearer-token: ${PEOPLE_FLOW_EVIDENCE_TOKEN}
    object-directory: ./data/people-flow/evidence
```

未配置 `bearer-token` 时 WVP 不应启动 exact 适配器。

必需请求头：

| Header | 语义 |
| --- | --- |
| `Authorization` | `Bearer <configured-token>`。适配器拒绝缺失、错误或空 token。 |
| `Idempotency-Key` | daemon outbox 的稳定键；适配器按不透明字符串持久化，不自行重算。 |
| `X-Evidence-ID` | 与 multipart 的 `evidence_id` 相同，用于快速路由和诊断。 |

multipart 字段：

| 字段 | 类型 | 约束 |
| --- | --- | --- |
| `event_id` | text | 非空，关联标准 Alarm 描述中的 `event_id=`。 |
| `evidence_id` | text | 非空；必须与 metadata 和 `X-Evidence-ID` 一致。 |
| `metadata` | text | 版本化 JSON，至少通过端侧 daemon 当前的 v2 身份、时间、帧和 SHA-256 校验。 |
| `image` | file | JPEG；文件名不作为身份，内容 SHA-256 以 metadata 为准。 |

当前 daemon 的发送实现位于
`gb28181_daemon/internal/delivery/evidence.go`。HTTP 2xx 表示本次请求已
接受；非 2xx 会进入 daemon 的独立证据重试状态，不影响 Alarm sink。

## 关联规则

标准 Alarm 继续使用现有字段：`CmdType`、`SN`、`DeviceID`、优先级、方式、
时间、描述和类型。当前描述包含类似：

```text
contract_version=2;event_id=<id>;phase=START;...;evidence_id=<id>
```

适配器从 WVP 已解析的标准 Alarm 事件中提取 `event_id`，再使用
`(device_id, channel_id, event_id)` 作为告警关联范围。`SN` 只属于 SIP
投递序号，不能作为事件身份；`frame_id`、PTS 和 `event_seq` 也不能替代
`event_id`。

WVP 侧的 `ExactEvidenceAlarmListener` 在 stock `DeviceAlarmEvent` 已解析后提取
`contract_version=2` 的 `event_id`，再使用独立的适配器存储记录以下关系，而不是修改 stock 的
`wvp_device_alarm` 语义：

```text
(device_id, channel_id, event_id)
    -> alarm_seen_at / alarm_time / alarm_type / alarm_description
    -> evidence_id / object_key / sha256 / frame_id / source_pts / state
```

当前 daemon 的 metadata 只携带 `channel_id`，标准 Alarm 发送时也将该 channel
作为 `<DeviceID>`；WVP stock 解析器另外把 SIP 发送方写入 `DeviceAlarmNotify.deviceId`。
因此当前适配器在 metadata 没有可选 `device_id` 时明确使用
`device_id = channel_id`，并在 Alarm 关联时使用线上的 `<DeviceID>`/`channel_id`，
与证据侧回退保持一致。后续若 daemon/上游补充真实 device ID，必须同时出现在
metadata 和 Alarm 关联键中，不能静默混用两套身份。

这样可以保留 stock Alarm 列表的兼容性，同时使图片先到时暂存为
`WAITING_ALARM`，Alarm 先到时使证据记录等待关联。

## 查询接口

适配器启用后，使用同一个 Bearer token 查询独立关联状态：

```text
GET /api/v1/people-flow/evidence?device_id=<id>&channel_id=<id>&event_id=<id>&evidence_id=<id>
```

成功返回 `result=found` 和 `state`、`object_key`、`sha256`、帧/PTS、Alarm
关联时间及失败原因等字段；记录不存在返回 `404`，身份不完整返回 `422`。
查询结果来自适配器自己的表，不代表 stock Alarm 页面已经展示端侧 JPEG。

同一身份的端侧 JPEG 可通过以下入口读取，响应为 `image/jpeg`；对象不存在时返回
`404`，不会返回服务器本地路径：

```text
GET /api/v1/people-flow/evidence/object?device_id=<id>&channel_id=<id>&event_id=<id>&evidence_id=<id>
```

## 幂等与状态

适配器按 `Idempotency-Key` 和 `(event_id, evidence_id)` 双重检查：

- 相同键、相同身份和相同 SHA-256 的重复请求返回成功，不重复生成文件或关联记录；
- 相同键但身份、metadata 或 SHA-256 不同返回冲突，不覆盖已接受内容；
- 同一 `event_id` 搭配不同 `evidence_id` 必须显式记录，不静默覆盖；
- 关联记录至少具有 `RECEIVED`、`WAITING_ALARM`、`ASSOCIATED`、`REJECTED`
  和 `EXPIRED` 状态；状态变化需可查询；
- 重启后依据适配器自己的持久存储恢复幂等键、证据文件和等待关联记录。

建议 HTTP 结果：

| 结果 | 含义 | daemon 行为 |
| --- | --- | --- |
| `2xx` | 已接收或幂等重复 | 标记 evidence sent。 |
| `401/403` | 认证失败 | 重试至上限并进入 dead，需修复配置。 |
| `409` | 幂等键冲突或身份冲突 | 重试至上限并保留冲突原因。 |
| `422` | metadata、图片或关联身份无效 | 重试至上限并保留校验原因。 |
| `410` | 证据已过期或租约失效 | 终止并标记 expired/dead。 |
| `5xx`/网络错误 | 适配器暂时不可用 | 按 daemon 有界退避重试。 |

## 到达顺序测试矩阵

WVP 侧适配器的本地测试先覆盖以下顺序，不需要启动 WVP、ZLM 或 SIP：

| 场景 | 预期 |
| --- | --- |
| evidence -> Alarm | 证据先进入 `WAITING_ALARM`，Alarm 到达后变为 `ASSOCIATED`。 |
| Alarm -> evidence | Alarm 先记录，证据到达后变为 `ASSOCIATED`。 |
| evidence 重复 | 同 SHA 幂等成功，存储只有一份。 |
| 同键不同内容 | 返回 `409`，原记录和原文件不变。 |
| Alarm 重复/SIP 重试 | 不重复创建 exact 关联；stock Alarm 行为单独记录。 |
| 适配器重启 | 已接收、等待和已关联状态可恢复。 |
| SHA/身份/过期错误 | 返回明确错误并进入可见失败状态。 |

## 当前未宣称事项

- 尚未修改 WVP stock 解析器或 `wvp_device_alarm` 数据库语义；新增表属于独立适配器。
- 尚未启动 WVP、ZLM、数据库或 SIP 运行环境。
- 尚未证明标准 Alarm 已在当前 WVP 实例入库，也尚未证明 exact 图片已在
  WVP 页面展示。
- 完成本文只代表适配边界明确，不代表 T8 或 T10 通过。
