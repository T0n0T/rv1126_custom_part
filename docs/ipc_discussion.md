# IPC 软件架构讨论清单（初稿）

> 状态：**初稿，待逐项确认/精修**
> 使用方式：逐项讨论后勾选或补充结论，作为架构文档的输入。

## 1. 目标与范围

- [ ] 产品定位：单路 IPC？是否必须本地预览？
- [ ] 功能范围：实时视频 / 音频 / 对讲 / 回放 / PTZ / 报警 / 抓图 / 校时，哪些进 V1
- [ ] GB28181 版本与兼容：2016 还是 2022；PS 还是 TS 封装；H.264/H.265；G.711 音频
- [ ] 并发模型：同时几路 INVITE、是否需要子码流、多平台同时观看
- [ ] 平台边界：只支持 RV1126B arm64，还是预留其他芯片/架构

## 2. 进程与整体架构

- [ ] 进程拆分：daemon（Go）+ media_engine（C）+ 本地 preview，是否接受
- [ ] 常驻 vs 按需：采集/编码是否开机常驻（影响 INVITE 响应速度和功耗）
- [ ] 崩溃恢复：谁拉起谁、重启后 SIP 状态如何恢复
- [ ] 与现有 rkipc / ipcweb 的共存关系、功能划分

## 3. 信令层（Go daemon）

- [x] 语言与库：Go + sipgo（MIT）确定，REGISTER + Digest 已与 WVP 联调通过（401→200）；`go-av/gosip` 仅作 GB28181 报文/流程参考（无 LICENSE，不引入代码）；panjjo/gosip 为平台侧实现，不采用
- [ ] GB 功能拆解：注册、心跳、目录、实时流、对讲、PTZ、报警、抓图、回放，逐一列实现清单
- [ ] 会话管理：SDP 生成、端口池、SSRC 分配、会话超时、BYE/重邀处理
- [ ] 配置模型：SIP 服务器、设备 ID、用户名密码、编码偏好、心跳间隔
- [ ] 本地 API：是否需要 HTTP 接口给 ipcweb/运维使用

## 4. 媒体层（C / GStreamer）

- [ ] 管线拓扑：tee 共享源（预览 + 编码）是否最终方案
- [ ] 硬件加速：dmabuf 采集、MPP 硬编、RGA 缩放/旋转、KMS 显示
- [ ] 编码参数：码率控制（CBR/VBR）、GOP、帧率策略、H.264 等级
- [ ] 音频：是否重编 base 开 alsa；G.711 还是 AAC
- [ ] 对讲 / 回放 / 抓图各自的管线形态
- [ ] 板级参数配置化：video 节点、connector/plane、分辨率默认值

## 5. daemon ↔ media 控制协议

- [ ] 传输：unix socket vs TCP vs 其他
- [ ] 编码：自定义 JSON vs protobuf/gRPC
- [ ] 消息集合与语义：start/stop_live、backchannel、snapshot、set_bitrate、get_status
- [ ] 版本协商与兼容策略：hello、proto_version、错误码表
- [ ] 事件上报：stats/error 的格式与频率

## 6. 构建、部署与固件集成

- [ ] Go 工具链如何进 SDK 构建（Makefile 拉取固定版本？CI 出二进制？）
- [ ] media_engine 从 gst_aiq_preview 演进的代码组织
- [ ] 固件布局与自启：RkLunch 顺序、/oem 目录结构
- [ ] 配置持久化：/oem（出厂默认）vs /userdata（用户修改）
- [ ] 日志与调试：gst_debug 是否开启、日志落盘策略

## 7. 非功能要求

- [ ] 内存/CPU 预算：Go daemon 常驻开销实测目标
- [ ] 延迟指标：预览到平台端到端延迟、INVITE 到首帧时间
- [ ] 长期稳定性：连续运行、断网重连、媒体断流恢复
- [ ] 安全：SIP 认证强度、本地接口权限、端口管理

## 8. 里程碑与验收标准

- [ ] Phase 1：单路实时流固定目标跑通
- [ ] Phase 2：注册 + INVITE 动态推流
- [ ] Phase 3：目录/PTZ/报警/抓图/对讲
- [ ] Phase 4：多路/子码流/音频/运维调优
- [ ] 每阶段验收标准（平台侧可注册、可看流、断线自动恢复等）

## 9. 待验证的技术风险

- [x] PS vs 裸 H.264 RTP：联调平台（WVP+ZLM）SDP 提供 PS(96)/H264(98)/H265(99)，V1 采用裸 H.264 RTP（`rtph264pay pt=98`），PS 留作第三方平台兼容备选
- [x] sipgo 对 GB28181 场景支持度：REGISTER + Digest 已验证；Keepalive MESSAGE、INVITE 待后续 spike
- [ ] RGA 重编后 videoconvert/scale 行为是否符合预期
- [ ] 板端音频通路（mic/喇叭）是否可用
- [ ] RV1126B 上 Go daemon + 全功能媒体引擎的总内存占用

## 10. 结论记录

- 每项确认后，在此记录结论与责任人（后续精修时补充）。
