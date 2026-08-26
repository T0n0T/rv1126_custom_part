# RV1126B IPC 软件架构设计（草案 v0.1）

> 状态：**草案，待讨论精修**
> 背景：在 `custom_part` 现有 GStreamer 集成（GLib/GStreamer 1.24.11 + gst-plugins + gstreamer-rockchip MPP 插件）之上，把 RV1126B 做成支持 GB28181 的 IPC。
> 目标：信令与媒体解耦、硬件加速优先、可长期稳定运行。

## 1. 总体进程模型

整体拆成三个独立进程，外加一个本地调试工具：

```
┌─────────────────────────────────────────────────────────┐
│                       固件 / OEM 分区                     │
│                                                         │
│  gb28181_daemon (Go, 静态二进制)                         │
│  ├─ SIP UA：REGISTER / Keepalive / INVITE / BYE          │
│  ├─ 会话管理器：INVITE → 会话 → 端口分配 → SDP           │
│  ├─ 设备能力：目录 / 报警 / PTZ / 抓图 / 对讲             │
│  └─ 本地控制接口：unix socket JSON-RPC + HTTP(可选)      │
│        │                                                │
│        │ /var/run/gb28181_media.sock                     │
│        ▼                                                │
│  media_engine (C/GStreamer，由 gst_aiq_preview 演进)      │
│  ├─ RKAIQ：ISP 初始化 / 对焦 / raw stream 控制           │
│  ├─ 采集：v4l2src (dmabuf)                              │
│  ├─ 硬编：mpph264enc / mpph265enc → PS/TS → RTP → UDP    │
│  ├─ 本地预览：tee → kmssink                              │
│  └─ 对讲 / 抓图 / 状态上报                               │
│                                                         │
│  gst_aiq_preview（保留，仅本地调试，不参与自启）            │
│  rkipc / ipcweb（现有业务共存，daemon 提供状态接口）       │
└─────────────────────────────────────────────────────────┘
```

进程职责：

| 进程 | 语言 | 职责 | 关键依赖 |
|---|---|---|---|
| gb28181_daemon | Go | SIP 信令、会话生命周期、GB XML、控制 API | sipgo、标准库 |
| media_engine | C | 摄像头 IQ + GStreamer 管线 + 硬件编解码 | RKAIQ、MPP、GStreamer |
| gst_aiq_preview | C | 本地单路预览调试 | 同 media_engine |
| 看门狗脚本 / init | sh | 拉起、崩溃重启 | RkLunch |

**边界原则**：daemon 不碰媒体数据，media_engine 不碰 SIP；二者只通过版本化 JSON 协议通信。

## 2. media_engine（C）内部模块

```
media_engine
├── aiq_ctrl        # 从现有 aiq_start/aiq_stop 提炼，含 AF
├── capture         # v4l2src io-mode=dmabuf，配置化 device/format
├── session_mgr     # 会话表：live/backchannel/snapshot，动态建/拆分支
├── gst_runner      # 现有 run_pipeline 演进：bus 监听、EOS/ERROR 上报
├── ipc_server      # unix socket 接收 daemon 命令，返回事件
└── hw_caps         # MPP/RGA/KMS 能力与参数（connector/plane 不再硬编码）
```

主管线（硬件加速版）：

```
v4l2src io-mode=dmabuf ! video/x-raw,format=NV12,width=1920,height=1080,framerate=30/1
  ! tee name=t
t. ! queue ! kmssink sync=false                          # 预览
t. ! queue ! rgarotate rotation=90 ! kmssink sync=false   # 预览（RGA 旋转）
t. ! queue ! mpph264enc bitrate=4096 ! h264parse
  ! rtph264pay pt=98 ! udpsink host=平台 port=会话端口    # GB28181 视频（V1）
```

旋转：图像相关参数全部留在 media_engine（C 侧），daemon 协议不携带。
板端屏幕预览用配置项 `preview_rotation`，推流用配置项 `stream_rotation`
（均为 0/90/180/270，RGA 硬件旋转，元素为 media_engine 内置的
`rgarotate`），两者相互独立。`rgarotate` 直接使用 librga（`/dev/rga`），
不需要重编 gstreamer-rockchip（SDK 该版本 `-Drga=disabled`，
`mpph264enc` 不带 rotation 属性）。

- 音频（可选）：`alsasrc ! audioconvert ! alawenc` 并入 mux；需要重编 base 开 alsa。
- 对讲（反向）：`udpsrc ! rtp depay ! alawdec ! audioconvert ! alsasink`，独立短管线。
- 抓图：tee 分支挂 `mppjpegenc` + appsink，或按需起临时管线。
- 负载选择：本地联调平台（WVP + ZLM）SDP 提供 PS(96)/H264(98)/H265(99)，V1 采用裸 H.264 RTP（`rtph264pay pt=98`）；若第三方平台仅支持 PS，再评估自写 `rtppspay`（详见 [ipc_interop.md](./ipc_interop.md)）。

**常驻策略**：开机即起采集 + 编码（AIQ 稳定、INVITE 秒回），INVITE 只开 UDP 输出，BYE 只停输出不停采集。

## 3. gb28181_daemon（Go）内部模块

```
gb28181_daemon
├── sip_ua          # sipgo：UDP/TCP、Digest 认证、事务
├── gb_parser       # GB XML：DeviceInfo/Catalog/Keepalive/PTZ/Alarm body
├── session_mgr     # dialog ↔ media session 映射、端口池、超时
├── media_client    # unix socket 客户端，命令/事件
├── config          # /oem/usr/share/gb28181.json
└── api             # 本地 HTTP/JSON，供 ipcweb 或运维查询状态
```

**选型结论**：SIP 传输/事务层使用 `sipgo`（MIT，已与 WVP 联调验证 REGISTER + Digest 401→200）；GB28181 报文/SDP 设计参考 `github.com/go-av/gosip`（仅作参考，不引入代码——仓库无 LICENSE 且为个人维护项目）。

配置项（JSON）：SIP 服务器地址/端口、设备 20 位 ID、用户名/密码、心跳间隔、编码偏好（H.264/H.265、码率、分辨率）、预览开关、SSRC 分配规则。

## 4. daemon ↔ media_engine 控制协议

### 4.1 形态

- 传输：`SOCK_STREAM` unix socket，路径 `/var/run/gb28181_media.sock`，权限 `0600`。
- 编码：compact JSON，换行分隔（单条上限 64KB）。
- 语义：借鉴 JSON-RPC 的 request / response / notification，字段按业务自定义。
- 协议版本：消息带 `v` 字段，连接时 `hello` 协商。

### 4.2 消息示例

请求：

```json
{"v":1,"id":12,"method":"media.start_live","params":{"session_id":"s1","codec":"h264","width":1920,"height":1080,"fps":30,"bitrate":4096,"dest_ip":"192.168.1.10","dest_port":20000,"ssrc":123456}}
```

响应：

```json
{"v":1,"id":12,"result":{"ok":true,"media_port":20000}}
```

错误：

```json
{"v":1,"id":12,"error":{"code":-32000,"message":"no free encoder channel"}}
```

事件（通知，无 id）：

```json
{"v":1,"method":"media.event","params":{"event":"stats","session_id":"s1","fps":30,"bitrate":4012}}
```

### 4.3 消息集合（初定）

| 方向 | 消息 |
|---|---|
| daemon→media | `start_live {session_id, codec, width, height, fps, bitrate, dest_ip, dest_port, ssrc}`（图像参数不经过 daemon，旋转由 media_engine 配置 `stream_rotation` 控制） |
| daemon→media | `stop_live {session_id}` |
| daemon→media | `start_backchannel` / `stop_backchannel` |
| daemon→media | `snapshot` / `set_bitrate` / `get_status` |
| media→daemon | `event {error/eos/stats}`，含 fps、实际码率 |

### 4.4 错误码表（初定）

| code | 含义 |
|---|---|
| -32600 | 消息格式错误 |
| -32000 | 媒体忙 / 无空闲编码通道 |
| -32001 | 会话不存在 |
| -32002 | 参数非法 |

### 4.5 可替换性

Go 侧 `media_client` 按 Transport / Codec / Methods 三层设计，业务代码只依赖语义方法：

```go
type Transport interface {
    Send([]byte) error
    Recv() ([]byte, error)
    Close() error
}

type Codec interface {
    Encode(v any) ([]byte, error)
    Decode(data []byte, v any) error
}
```

- 换编码：JSON → protobuf/msgpack，新增 Codec 实现，业务零改动。
- 换传输：unix socket → TCP/D-Bus，新增 Transport 实现。
- 约束：C 侧 `ipc_server` 同步修改；两端以协议文档为契约，语义字段保持稳定。

## 5. 核心数据流

1. **注册**：daemon REGISTER（Digest）→ 200 → 每 N 秒 Keepalive XML；失败指数退避。
2. **实时流**：平台 INVITE → daemon 建会话、分配端口 → media_engine 启动 RTP 输出 → 200 OK 带 SDP → ACK → 推流；BYE → 停输出。
3. **对讲**：平台 INVITE（音频反向）→ media_engine 起反向管线。
4. **PTZ / 报警**：daemon 解析 XML → PTZ 转给 AIQ/ISP 控制；报警由 media_engine 或上层触发 → daemon 上报。
5. **心跳 / 异常**：媒体断流事件 → daemon 按标准处理（500/BYE 或重邀）；信令断线 → 重注册，媒体会话标记失效。

## 6. 构建与部署

### 6.1 现有构建改动点

- `custom_part/gstreamer/build.sh`：保持现状（rockchip 插件 `-Drga=disabled`）。
  RGA 旋转由 media_engine 内置 `rgarotate` 元素直接调用 librga 完成。
- gst-plugins-base：重开 `alsa`（若 V1 带音频）。
- gstreamer core：调试期开 `gst_debug`（当前 `-Dgst_debug=false`）。

### 6.2 新组件构建

- media_engine：由 `custom_part/gst_aiq_preview` 演进，沿用 `project/app` 转发桩模式。
- daemon：`custom_part/gb28181_daemon`，Go module + vendor，`GOOS=linux GOARCH=arm64 CGO_ENABLED=0`，产物拷入 `output/out/app_out/bin`。

### 6.3 固件布局（初定）

```
/oem/usr/bin/gb28181_daemon
/oem/usr/bin/media_engine
/oem/usr/lib/gstreamer-1.0/...
/oem/usr/share/iqfiles/...
/oem/usr/share/gb28181.json
/userdata/gb28181.json   # 用户覆盖配置（可选）
```

### 6.4 启动顺序

`RkLunch.sh` 拉起 `gb28181_daemon` → daemon 拉起 `media_engine`；daemon 崩溃由 init 脚本重启，media_engine 崩溃由 daemon 重启并重新建立会话。

### 6.5 联调平台（WVP + ZLMediaKit）

本地联调使用 `wvp-baseline` 封装环境（WVP-GB28181-Pro 2.7.4 + ZLMediaKit），SIP 端口 8160，ZLM 收流 RTP 10003（或动态 30000–30500），负载 PT：PS=96 / H264=98 / H265=99。平台启动方式、设备注册参数与 WVP API 验收流程见 [ipc_interop.md](./ipc_interop.md)。

## 7. 稳定性设计

- daemon 与 media_engine 独立崩溃域，重启互不影响核心状态。
- media_engine 常驻采集：IPC 抖动只清会话输出，不动摄像头状态。
- 看门狗：init 层守护 daemon；daemon 通过心跳/命令超时守护 media_engine。
- 控制协议幂等：`stop_live` 对不存在会话也返回明确结果。
- 日志：daemon 与 media_engine 各自落盘到 `/data/log`（或 syslog），带时间戳与 session_id。

## 8. 里程碑

1. **Phase 1**：media_engine 单路实时流 + 固定 UDP 目标跑通（复用现有代码，改 dmabuf/RGA）。
2. **Phase 2**：daemon 注册/心跳/INVITE → 控制 media_engine，平台可看实时画面。
3. **Phase 3**：目录、PTZ、报警、抓图、对讲。
4. **Phase 4**：子码流/多会话、音频、断线重连、运维接口、内存/延迟调优。

## 9. 风险与待验证

- 第三方平台若仅支持 PS over RTP，需要自写 payloader；本地 WVP+ZLM 联调平台已验证支持裸 H.264 RTP（pt=98），V1 不阻塞。
- ~~sipgo Digest 认证~~ 已通过 REGISTER spike 与 WVP 验证（401 → 200）；重传、超时、长连接行为仍待后续测试。
- WVP 注册成功后会立即向设备 Contact 推送 Catalog 查询 MESSAGE，daemon 必须实现 UAS（OnRequest）监听，不能只做客户端。
- RGA 旋转元素（rgarotate）在预览与推流分支的缓冲对齐/DDC 行为是否持续
  符合预期（已按 16 字节 stride 对齐输出）。
- 板端音频通路（mic/喇叭）可用性。
- RV1126B 上 Go daemon（约 10~20MB）+ 全功能媒体引擎的总内存占用需实测。
- 板级参数（video 节点、connector/plane）全部配置化，避免换板改代码。

## 10. 开放问题

- V1 是否包含音频、对讲、回放。
- 平台方接受的封装（PS/TS）与负载格式（H.264/H.265）。
- 是否需要子码流与多平台并发。
- 与现有 rkipc / ipcweb 的功能边界。

详细决策点见 [ipc_discussion.md](./ipc_discussion.md)。
