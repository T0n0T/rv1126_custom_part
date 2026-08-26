# 08 — 精确证据 Sink

**What to build:** 在启用 `exact_evidence` 时，通过独立 HTTP 通道传输可校验、可重试、可幂等关联的 JPEG 证据。

**Blocked by:** 06 — 证据缓存与持久事件桥；07 — daemon Outbox 与标准 Alarm。

**Status:** blocked

- [ ] 证据内容在确认前具有持久副本或有效保留租约。
- [ ] HTTP 认证、校验和、幂等键、重试退避和永久失败状态可测试。
- [ ] 图片和 Alarm 可任意顺序到达，证据失败不阻塞 Alarm 投递。
- [ ] 默认 stock 模式不启用该 Sink，也不改变标准 Alarm 契约。
