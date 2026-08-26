# gb28181-debug（模拟推流工具）

**开发期调试工具，不参与固件构建。**

在 `media_engine`（C/GStreamer + MPP）落地之前，用它顶替媒体引擎：
把 H.264 测试画面（`testsrc2` → libx264）按 GB28181 的 RTP 参数推给平台
（WVP + ZLMediaKit），用来验证“信令协商 → RTP 推流 → 平台出流”通路。

信令侧由 `gb28181_daemon` 负责；本工具只负责**推流**，不处理 SIP。

## 用法

```sh
go build -buildvcs=false -o gb28181-debug .

./gb28181-debug \
  -dest 192.168.1.88:10003 \
  -ssrc 200004568 \        # 与 INVITE 的 y=0200004568 数值一致（十进制）
  -pt 98 \
  -size 640x360 -fps 25 -bitrate 800 \
  -duration 30            # 0 = 一直推，Ctrl+C 停止
```

常用参数：

| 参数 | 说明 | 默认 |
|---|---|---|
| `-dest` | 平台收流地址（INVITE SDP 里的 c=/m= 行） | `192.168.1.88:10003` |
| `-ssrc` | RTP SSRC，十进制，与 INVITE `y=` 一致 | `200004568` |
| `-pt` | RTP payload type，H.264 用 98 | `98` |
| `-size` / `-fps` / `-bitrate` | 测试画面尺寸/帧率/码率 | 640x360 / 25 / 800 |
| `-duration` | 推流秒数，0 为持续推 | `0` |
| `-ffmpeg` | ffmpeg 路径 | `ffmpeg` |

## 与 daemon 的分工

- `gb28181_daemon`：REGISTER / 心跳 / Catalog / INVITE / BYE 等信令，媒体侧只
  保留 `Controller` 接口（`none` 或 `rpc`），不内置任何推流实现。
- `gb28181-debug`：手动模拟媒体引擎，推真实 RTP 到平台；`media_engine` 就绪后
  本工具退役。

## 退役条件

`media_engine` 实现 `media.start_live` 等 unix socket RPC 后，daemon 切
`media.mode: rpc`，本工具不再需要。
