# gb28181_daemon

RV1126B IPC 的 GB28181 信令守护进程（Go）。当前为**骨架版本**：SIP 注册、
心跳、Catalog/DeviceInfo 应答、INVITE/BYE 会话管理都已接线，媒体侧通过
`media.Controller` 接口解耦，等待 `media_engine`（C/GStreamer）落地后接入
unix socket RPC。

## 目录结构

```text
cmd/gb28181-daemon/  入口：加载配置、信号处理
internal/
  app/               编排层：唯一了解所有模块的地方
  config/            配置加载与校验
  gbxml/             GB/T 28181 XML 报文（解析与构造）
  media/             媒体控制接口 + unix socket JSON RPC 客户端 + Noop
  session/           直播会话注册表（Call-ID -> Live）
  sipua/             sipgo 封装：UAC 注册/Digest、UAS 消息/INVITE/BYE
configs/             示例配置
```

依赖方向保持单向：`app -> {config, gbxml, media, session, sipua}`，`sipua`
不依赖 GB XML 和媒体实现，`media` 不依赖 SIP。

## 构建与测试

```sh
make build          # 本机构建，产物 bin/gb28181-daemon
make test           # go test ./...
make vet            # go vet ./...
```

交叉编译（RV1126B，arm64，静态二进制）：

```sh
GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -buildvcs=false -o bin/gb28181-daemon ./cmd/gb28181-daemon
```

## 运行

```sh
./bin/gb28181-daemon -config configs/gb28181.example.json
```

配置文件说明：

- `sip`：设备/平台标识、SIP 服务器、Digest 密码、注册与心跳参数。
- `media.mode`：`none`（无媒体引擎，INVITE 会被拒绝）或 `rpc`（unix socket
  JSON 协议，见 `docs/ipc_architecture.md`）。
- `media.simulate=true`：Noop 控制器假装成功，用于在没有 media_engine 时
  联调 WVP 的完整 SIP 闭环（注册→目录→INVITE→ACK→BYE）。
- `stream`：INVITE 协商后下发给媒体引擎的编码参数。
- `channels`：目录上报的通道列表，INVITE 只接受列表内的通道。

## IO 控制（测试桩）

当前实现了一个测试用的设备侧 IO 控制：收到平台下发的 `<Control>` 信令时
**只解析并打印到 stdout，不操作真实 GPIO**。辅助开关（GB/T 28181-2016
A.3.7，PTZCmd 字节 4 为 `8CH` 开 / `8DH` 关，字节 5 为开关编号）触发时打印：

```text
[gb28181-io-test] 2026-08-13 14:30:00.123 IO1 ON (sn=11)
```

示例信令（开 1 号辅助开关）：

```xml
<Control>
  <CmdType>DeviceControl</CmdType>
  <SN>11</SN>
  <DeviceID>35020000001320000001</DeviceID>
  <PTZCmd>A50F008C01000041</PTZCmd>
</Control>
```

`GuardCmd`（布防/撤防）、`AlarmCmd`（报警复位）、`RecordCmd`（录像控制）也会
打印并回发 `<Response><Result>OK</Result></Response>`；其余 DeviceControl
子命令仅记日志。后续接入真实 GPIO 时替换 `internal/app/app.go` 里的
`controlHandler` 打印即可。

## 当前状态

- [x] REGISTER + Digest（WVP 2.7.4 已验证）
- [x] Catalog / DeviceInfo 应答（WVP 已验证，通道可入库）
- [x] Keepalive 心跳、失败重注册、周期重注册
- [x] INVITE → 200(SDP) → ACK → 启动媒体 → BYE → 停止媒体（sipua 层完成，
      媒体实现待接入；spike 中用 ffmpeg 验证过同一流程）
- [x] DeviceControl（IO 测试桩）：辅助开关 PTZCmd 8CH/8DH 触发时打印到 stdout
- [ ] media_engine RPC 服务端（C/GStreamer）
- [ ] 控制协议正式定稿（hello/版本协商、错误码表）
- [ ] PTZ / 报警 / 抓图 / 对讲（真实 IO/GPIO 接入）

## 参考

- 架构与协议：`custom_part/docs/ipc_architecture.md`
- 联调平台与验收流程：`custom_part/docs/ipc_interop.md`
- 模拟推流调试工具（ffmpeg RTP）：`custom_part/tools/gb28181-debug/`
