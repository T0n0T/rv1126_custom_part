# 07 — daemon Outbox 与标准 Alarm

**What to build:** 让 GB28181 daemon 能可靠接收事件、持久化投递状态并向上级平台发送标准 Alarm MESSAGE。

**Blocked by:** 06 — 证据缓存与持久事件桥。

**Status:** blocked

- [ ] daemon 重启后可从最后确认游标继续接收，重复事件按幂等键处理。
- [ ] Outbox 分别记录 Alarm 和证据 sink 状态，成功 sink 不因另一 sink 失败而重发。
- [ ] 标准 Alarm XML 包含必需字段，`SN`、事件序号、帧 ID 和 PTS 严格分离。
- [ ] Alarm XML 不嵌入 JPEG、Base64 或端侧本地路径；默认只投递 START。
- [ ] SIP 超时、拒绝、未注册和重试耗尽均可观测且不影响媒体主链路。
