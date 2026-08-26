# 13 — exact-evidence 发布验收

**What to build:** 在 stock 基线验收通过后，完成精确证据链路的部署级验收和降级判定。

**Blocked by:** 11 — exact-evidence WVP 适配器；12 — stock WVP 真板发布验收。

**Status:** blocked

- [ ] 验证触发帧到证据内容的帧 ID、PTS、时间和校验关联。
- [ ] 验证 HTTP 断线、重复请求、到达乱序、保留上限和失败重试。
- [ ] 证明 exact 失败时可按策略降级为 Alarm-only 或 stock 拉图，并明确标记。
- [ ] 形成 exact-evidence 版本的发布、回滚和运维说明。
