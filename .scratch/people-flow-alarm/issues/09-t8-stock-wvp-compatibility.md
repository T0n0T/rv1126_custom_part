# 09 — stock WVP 兼容验证

**What to build:** 在不修改 WVP 解析器的情况下接收标准 Alarm，并明确平台异步抓图只是告警附近帧。

**Blocked by:** 07 — daemon Outbox 与标准 Alarm。

**Status:** blocked

- [ ] stock WVP 接受并保存标准 Alarm，字段映射和类型符合目标版本行为。
- [ ] 重复投递不会造成不可控的消息风暴，SIP 重试结果可追踪。
- [ ] WVP 生成的图片与端侧触发帧差异被记录为兼容模式限制。
