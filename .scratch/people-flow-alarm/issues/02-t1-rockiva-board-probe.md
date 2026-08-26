# 02 — 真板 RockIVA 探针

**What to build:** 在真实 RV1126B 上运行一个不属于产品路径的 RockIVA 探针，确认人体检测、视频跟踪和帧所有权可以稳定工作。

**Blocked by:** 01 — 基线与回滚开关。

**Status:** blocked

- [ ] 在候选分辨率和采样率下取得稳定 person 检出及 `objId/state` 生命周期。
- [ ] 固化经过测量的模型、结果模式、核心掩码、帧格式和输入方式。
- [ ] 证明 push、release callback、停止和重启无泄漏、重复释放或悬空引用。
- [ ] 证明分析分支关闭点播后仍可运行，且启停不影响主视频。
