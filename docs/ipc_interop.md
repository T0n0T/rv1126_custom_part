# GB28181 联调平台说明（WVP + ZLMediaKit）

> 状态：初稿
> 用途：作为 `gb28181_daemon` / `media_engine` 的联调与自动化验收平台。
> 仓库路径：`/home/Tiger/Documents/code/web/wvp-baseline`

## 1. 平台组成

- `WVP-GB28181-Pro` 2.7.4：SIP 信令服务器 + 管理 API（PostgreSQL/Redis）。
- `ZLMediaKit`：RTP 收流、转封装（RTSP/RTMP/FLV/WebRTC），hook 回 WVP。
- Nginx：前端 8080。
- `baseline.sh`：封装 configure / build / start / stop / status。

## 2. 当前运行状态与启动方式

- `.runtime/` 上次配置 IP 为 `192.168.1.80`（2026-05-29），当前机器 IP 已变化，且容器当前**未运行**。
- 启动前需重新配置并拉起：

```bash
cd /home/Tiger/Documents/code/web/wvp-baseline
./baseline.sh configure <当前机器IP>
./baseline.sh start
./baseline.sh status
```

- 注意：该仓库 AGENTS.md 要求分步执行并在每步停下等人工确认，不要自主连跑多步。

## 3. 关键参数

| 项 | 值 |
|---|---|
| SIP 服务器 | `<host>:8160`（UDP/TCP） |
| SIP 域 | `3502000000` |
| 平台 SIP ID | `35020000002000000001` |
| 设备注册密码 | `wvp_sip_password` |
| WVP Web / API | `http://<host>:8080`、`http://<host>:18978` |
| WVP 默认账号 | `admin / admin` |
| ZLM RTP 收流 | `10003` UDP（或动态 30000–30500） |
| ZLM 其他端口 | RTSP 10002、RTMP 10001、RTC 8000 |
| 负载类型 | `ps_pt=96`、`h264_pt=98`、`h265_pt=99` |

## 4. 协议交互要点

### 4.1 注册

- 设备需**先添加到 WVP**（API/界面），WVP 按 `deviceId` 校验，未入库设备注册会被拒绝。
- REGISTER 使用 Digest 认证（RFC 3261），密码为 `wvp_sip_password`。
- 注册成功后按配置周期发送 Keepalive（MESSAGE，XML body）。

### 4.2 点播（INVITE）

WVP 发送 INVITE，SDP 关键内容（源码核对）：

```text
m=video <ZLM收流端口> RTP/AVP 96 126 125 99 34 98 97
a=recvonly
a=rtpmap:96 PS/90000
a=rtpmap:126 H264/90000
a=rtpmap:125 H264S/90000
a=rtpmap:99 H265/90000
a=rtpmap:98 H264/90000
a=rtpmap:97 MPEG4/90000
y=<ssrc>
```

- 设备在 200 OK 的 SDP 中选择负载类型并回传自己的 `y=<ssrc>` 等字段。
- WVP 收到 ACK 后开启收流（`push-stream-after-ack: true`）。
- 设备向 INVITE SDP 中 `m=video` 行给出的 IP:端口推 RTP。

### 4.3 V1 负载选择

- 采用裸 H.264 RTP（pt=98）：`mpph264enc ! h264parse ! rtph264pay pt=98 ! udpsink`，无需 PS/TS 封装。
- PS（pt=96）留作第三方平台兼容备选；若后续必须支持 PS，再评估自写 `rtppspay`。

### 4.4 BYE / 异常

- BYE 后设备停止推流并清理会话。
- 媒体断流由 ZLM hook 通知 WVP；设备侧由 daemon 管理重连/重邀。

## 5. WVP API 自动化验收流程

登录（密码为明文 MD5 小写）：

```http
GET /api/user/login?username=admin&password=<md5("admin")>
```

后续请求携带 `access-token: <login data.accessToken>`（基线配置 `interface-authentication: false` 时可简化，但脚本按带 token 编写）。

推荐闭环：

| 步骤 | 接口 | 说明 |
|---|---|---|
| 登录 | `GET /api/user/login` | 取 token |
| 添加设备 | `POST /api/device/query/device/add` | JSON `Device`，必填 `deviceId` |
| 查询设备在线 | `GET /api/device/query/devices?query=<deviceId>` | 确认 REGISTER 后 `onLine=true` |
| 目录同步 | `GET /api/device/query/devices/{deviceId}/sync` | 触发设备上报目录 |
| 通道查询 | `GET /api/device/query/devices/{deviceId}/channels` | 取 `channelId` |
| 发起点播 | `GET /api/play/start/{deviceId}/{channelId}` | 异步 INVITE，返回 `StreamContent` |
| 校验出流 | `GET /api/server/media_server/media_info` | 确认流在线 |
| 停止点播 | `GET /api/play/stop/{deviceId}/{channelId}` | 发 BYE |

设备/通道 ID 示例（可配置，勿写死）：

```text
设备 ID:   35020000001320000001
通道 ID:   35020000001310000001
```

## 6. 设备侧测试清单（Phase 2 / 3）

- [ ] REGISTER 成功，WVP 设备列表显示在线
- [ ] Keepalive 周期上报稳定（默认 60s）
- [ ] WVP 点播后，ZLM 收到 RTP，`ws_flv` 可播放
- [ ] BYE 后推流停止、会话与端口释放
- [ ] media_engine 崩溃 → daemon 自动拉起并恢复推流
- [ ] 断网 → SIP 重注册、媒体会话恢复
- [ ] WVP 侧目录同步能拿到通道信息

## 7. 参考

- `wvp-baseline/README.md`、`docs/02-build-and-deploy.md`、`docs/04-wvp-api-reference.md`
- 本仓库：[ipc_architecture.md](./ipc_architecture.md)、[ipc_discussion.md](./ipc_discussion.md)
