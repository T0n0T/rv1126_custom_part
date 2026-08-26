# media_engine (C/GStreamer)

GB28181 IPC 的媒体执行端：接收 `gb28181_daemon` 的 unix socket JSON 命令，
负责 RKAIQ、V4L2 采集、硬件编码（MPP）与 RTP 推流。由 `gst_aiq_preview`
演进而来，模块分层：

```
media_engine
├── src/
│   ├── main.c                入口：配置 → engine → ipc_server → GLib 主循环
│   ├── common/               公共工具：日志/字符串/错误码（util, me_errors）
│   ├── config/               配置模块：配置文件 + 命令行解析
│   ├── ipc/                  unix socket JSON-RPC 服务（已完成）
│   ├── session/              会话表与生命周期（已完成，LiveBackend 解耦）
│   ├── gst/                  GStreamer 硬件加速管线（已完成：常驻采集 +
│   │                         动态 RTP 分支 + bus 事件上报；抓图待补）
│   │                         含 me_rga_rotate：librga 硬件旋转元素
│   │                         （rgarotate，预览/推流共用，替代 videoflip）
│   ├── aiq/                  RKAIQ init/prepare/start/stop + 对焦（已完成，
│   │                         失败不阻断 IPC，start_live 回传原因）
│   └── engine/               组合根：装配 LiveBackend、配置级校验
├── tests/                    宿主机单测（假 LiveBackend）
├── third_party/
│   ├── cJSON/                单文件 JSON 解析（MIT）
│   └── libyaml/              静态链接 YAML 解析（MIT, 0.2.5）
├── config/media_engine.yaml.example  配置样例（YAML 子集）
├── Makefile / README.md
└── out/                      构建产物
```

分层约定：`ipc → engine（组合根）→ session_mgr / gst_runner / aiq_ctrl`。
公共错误码在 `src/common/me_errors.h`；`session_mgr` 通过 `LiveBackend`
（函数指针 + 不透明 data）驱动 gst_runner，不依赖 GStreamer 类型，便于
单测替换假后端。模块间 include 一律走 `src/` 相对路径，例如
`#include "ipc/ipc_server.h"`。

## 构建

```sh
# 直接在 custom_part/media_engine 下构建
make

# 生成 clangd/clang-tidy 使用的编译数据库（需要 Bear，会执行一次完整构建）
make compile_commands

# 或通过 SDK project/app 转发桩构建
cd ../../project/app/media_engine && make
```

构建方式对齐 `gst_aiq_preview`：`RK_APP_PARAM` + 交叉工具链
（`aarch64-rockchip1240-linux-gnu`），链接 gstreamer/glib/rkaiq/rockit/librga，
产物在 `out/bin/media_engine`；编译数据库输出到当前目录的
`compile_commands.json`。`rgarotate` 是 media_engine 内置元素
（GstBaseTransform + librga 老版 C API，与 SDK gstreamer-rockchip 的 RGA
用法一致），不需要重编 `custom_part/gstreamer` 源码树。

## 运行与配置

所有硬件参数（video 节点、分辨率、fps、connector/plane、iq_dir、预览开关）
走配置文件或命令行，无硬编码。默认配置文件
`/oem/usr/share/media_engine.yaml`，样例见 `config/media_engine.yaml.example`。
配置为完整 YAML 语法（libyaml 0.2.5 静态链接，源码在
`third_party/libyaml/`，MIT）。schema 为顶层映射：已知键取标量值，未知键
告警忽略，已知键的值若是映射/序列则报错：

```yaml
cam_id: 0
iq_dir: /oem/usr/share/iqfiles
device: /dev/video24
format: NV12
width: 3840
height: 2160
fps: 30
connector_id: 97
plane_id: 75
preview: on        # on/off/true/false/yes/no/1/0
preview_rotation: 0  # 板端屏幕预览旋转：0/90/180/270（RGA 硬件旋转，
                     # 仅预览分支）
preview_width: 480   # 预览输出宽度（RGA 缩放到屏幕分辨率）
preview_height: 800  # 预览输出高度
stream_rotation: 0  # 推流旋转：0/90/180/270（RGA 硬件旋转，仅推流分支；
                     # 图像参数只留在 C 侧，daemon 协议不携带）
af_mode: off
socket: /var/run/gb28181_media.sock
snapshot_dir: /data/media_engine/snapshots
```

```sh
./media_engine --config /oem/usr/share/media_engine.conf
./media_engine -d /dev/video24 -W 3840 -H 2160 -r 30 --preview on -s /tmp/me.sock
```

常用选项：`-a` iq_dir、`-c` cam_id、`-d` 设备、`-f` 格式、`-W/-H/-r` 分辨率帧率、
`-C/-P` KMS connector/plane、`--preview on|off`、`--preview-rotation 0/90/180/270`、
`--preview-width/--preview-height`、
`--stream-rotation 0/90/180/270`、
`--af` 对焦模式、`-s` socket 路径、`--snapshot-dir`。运行时会自动设置
`GST_PLUGIN_PATH=/oem/usr/lib/gstreamer-1.0`。

## 控制协议（与 daemon 契约对齐）

传输：`SOCK_STREAM` unix socket，compact JSON，换行分隔，单条 ≤ 64KB。
字段名与 `gb28181_daemon/internal/media/protocol.go`、`rpc.go` 严格一致。

请求/响应：

```json
{"v":1,"id":12,"method":"media.start_live","params":{"session_id":"s1","channel_id":"35020000001310000001","codec":"h264","width":3840,"height":2160,"fps":30,"bitrate":8192,"dest_ip":"192.168.1.88","dest_port":10003,"ssrc":"123456789","payload_type":98}}
{"v":1,"id":12,"result":{"ok":true}}
```

方法：

| 方法 | params | result |
|---|---|---|
| `media.ping` | - | `{"ok":true}` |
| `media.start_live` | session_id, channel_id, codec, width, height, fps, bitrate, dest_ip, dest_port, ssrc, payload_type | `{"ok":true}` |
| `media.stop_live` | session_id | `{"ok":true}` |
| `media.snapshot` | channel_id | `{"ok":true}` |
| `media.get_status` | - | `{"running":bool,"fps":int,"bitrate":int}` |

错误码：`-32600` 格式/未知方法、`-32000` 媒体忙或媒体错误、`-32001` 会话不存在、
`-32002` 参数非法。`ssrc` 按十进制 uint32 解析（对应 SIP SDP 的 `y=` 值）。

事件通知（无 id，尽力投递到当前已连接客户端）：

```json
{"v":1,"method":"media.event","params":{"event":"error","session_id":"s1","message":"..."}}
```

## 手工验证

```sh
./media_engine -s /tmp/me.sock &
printf '%s\n' '{"v":1,"id":1,"method":"media.ping"}' | socat - UNIX-CONNECT:/tmp/me.sock
printf '%s\n' '{"v":1,"id":2,"method":"media.get_status"}' | socat - UNIX-CONNECT:/tmp/me.sock
printf '%s\n' '{"v":1,"id":3,"method":"media.start_live","params":{"session_id":"s1","channel_id":"ch1","codec":"h264","width":3840,"height":2160,"fps":30,"bitrate":8192,"dest_ip":"192.168.1.88","dest_port":10003,"ssrc":"123456789","payload_type":98}}' | socat - UNIX-CONNECT:/tmp/me.sock
```

推流旋转：由 media_engine 配置 `stream_rotation` 控制（默认 0 不旋转），
非 0 时推流分支在编码前插入 `rgarotate`（RGA 硬件旋转），与板端屏幕预览的
`preview_rotation` 相互独立，可分别设置。图像相关参数全部留在 C 侧，
`start_live` 协议不携带。

## 单元测试（宿主机）

```sh
make -C tests
```

`tests/` 下宿主机单测：
- `session_mgr_test`：假 `LiveBackend` 验证会话生命周期（单会话互斥、重复
  id 拒绝、backend 失败传播、stop 失败仍强制清理、free 停残留会话）
- `config_test`：libyaml 配置解析（注释/引号/布尔/锚点别名/块标量、未知键
  忽略、非映射根/已知键复杂值报错）
- `aiq_ctrl_stub.c`：宿主机专用 AIQ 桩（仅测试构建用，不参与目标编译）

## 板端验证（RV1126B 实测）

`tests/board/` 下为板端测试工具（rpc_client、live 测试脚本、解析/链接探针、
gdb 脚本），均不进产品代码。已验证：

- AIQ（imx415）+ v4l2 dmabuf 采集 + kmssink 预览 + MPP 硬编全链路正常
- `start_live` 后 RTP 包到达 `192.168.1.88:20000`：v=2、pt=98、
  SSRC=123456789（十进制）、约 1800 包/5s（30fps）
- `stop_live` 后 udpsink 释放、包停止；连续 5 轮 start/stop 无异常
- WVP 端到端（daemon `media.mode: rpc`）：WVP 点播 → INVITE/ACK →
  板端 RTP → ZLM 收流（FLV/RTSP 可播，H264 3840x2160@30）→ WVP 停止 →
  BYE → stop_live → ZLM 流下线
- `preview: off` 时主管线自动挂 `fakesink` 空分支：本 SDK 构建的 tee
  从零输出 pad 启动时，后续动态请求的 pad 收不到缓冲（表现为预览关闭后
  点播无帧）；空分支让 tee 始终有已链接 pad，采集继续、屏幕不上屏
- 期间修复的坑：`gst_parse_launch` 只认双引号；mpph264enc 码率属性是
  `bps`（未知属性会被解析器静默丢弃）；本 SDK GStreamer 的 pad link 默认
  含 HIERARCHY 检查（动态分支须用 `GST_PAD_LINK_CHECK_NOTHING`）；
  **`gst_bin_new` 返回 floating 引用，`gst_bin_add` 会沉没该引用，必须显式
  `gst_object_ref` 再在销毁时 unref**（此前导致 stop 后
  `gst_object_unref ref_count > 0` critical）；**probe 回调里 release+unref
  tee 请求 pad 会导致流线程 use-after-free，第一次 stop 后 tee 死掉、预览
  冻结、后续会话无帧**（改用 tee sink pad 的 stream lock 在主线程安全拆链，
  分支 queue 加 `leaky=downstream` 防止慢网络堵死 tee）

## 实施状态

- [x] 模块 1：`ipc_server`（协议解析、字段校验、错误码、事件广播、单进程 GLib 主循环）
- [x] 模块 2：`session_mgr` 会话表与生命周期（LiveBackend 解耦 + 宿主单测）
- [x] 模块 3：`gst_runner` 硬件加速管线（v4l2src dmabuf → tee → kmssink；
      start_live 动态挂 `mpph264enc → h264parse → rtph264pay(pt/ssrc) →
      udpsink`；stop_live 阻塞解链释放 udpsink；bus 错误经 media.event
      上报；抓图暂缓）
- [x] 模块 4：`aiq_ctrl` RKAIQ 集成（enumStaticMetas/preInit_scene/sysctl
      init/prepare/start、raw stream 控制、对焦模式；engine_init 先起 AIQ
      再起采集管线）

当前 `snapshot` 仍为占位（返回 `-32000`），其余媒体动作已由 gst_runner
实现。四个模块已全部落地，并在板端 + WVP 完成端到端验收。
