# 11 — exact-evidence WVP 适配器

**What to build:** 为已适配的 WVP 提供独立证据接收和告警关联，使图片先到或后到都能正确归档。

**Blocked by:** 07 — daemon Outbox 与标准 Alarm；08 — 精确证据 Sink。

**Status:** blocked

- [ ] 通过事件和证据标识幂等关联标准 Alarm 和 HTTP 证据。
- [ ] 支持图片先到、Alarm 先到、重复请求和重启恢复。
- [ ] 关联失败、校验失败和过期证据具有明确的可见状态。
- [ ] 不要求在 GB28181 Alarm XML 中加入非标准图片字段。
