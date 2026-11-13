# 板载摄像头取放事件与屏幕调试

`board/out/rknn_yolo26_infer --camera /dev/video25` 在 RV1126B 本机完成 AIQ、采集、INT8 推理、BoT-SORT、区域穿越事件和 KMS 显示。预览与推理独立运行，ADB 负责部署、查询和结果拉回。默认仍使用 `comp-2-freeze10` 概率/坐标独立量化的双输出版本；也支持本项目已导出的单类别 YOLOv8 六头 INT8 模型，按运行时张量契约选择后处理。

## 代码与配置入口

| 文件 | 职责 |
| --- | --- |
| `board/rknn_yolo26_infer.c` | 主入口、参数、RKNN 推理、双输出后处理、检测 JSONL |
| `board/flow_camera.c` | AIQ、采集、独立预览线程、最新检测帧交接、KMS 生命周期与统计 |
| `board/flow_dma.c` | DMA heap 输出池、RGA 旋转/缩放、DMA-BUF 缓存同步 |
| `board/flow_events.cpp` | C/C++ 适配、BoT-SORT 与 CrossingEngine、轨迹/事件/摘要落盘 |
| `board/flow_image.c` | 检测输入 letterbox、三区/ID/事件文字、干净标定截图 |
| `board/flow_yolov8.c` | 六头 INT8 属性校验、按查询量化参数做 DFL/NMS、坐标还原 |
| `tracking/src/crossing_engine.cpp` | 取出/放入状态机、观测缺口与不确定原因 |
| `board/camera.env` | 设备、旋转、屏幕、置信度和区域 profile 路径 |
| `board/camera-zones-debug.json` | 明确未标定的示例三区，便于接线调试 |
| `board/calibrate_camera_zones.py` | 在板载摄像头干净截图上交互标定实际三区 |
| `board/run_rknn_camera_demo.sh` | 部署及 start/status/stop、保留独立运行证据 |

本增量仅修改 `flow_demo/`，生产 `media_engine` 与 GB28181 源码不在改动范围。

## 预览优化与坐标

- 板端 `192.168.1.63:5555`；ADB server 端口 5038，可用 `ADB_SERIAL`/`ADB_SERVER_PORT` 覆盖。
- AIQ camera 0、IMX415，IQ 目录 `/oem/usr/share/iqfiles`。初始化对齐 `media_engine/src/aiq/aiq_ctrl.c`：normal/day 场景、normal 工作模式、自动对焦，AIQ 持有 raw pipe；成功后打印 `AIQ_READY`。ISP selfpath `/dev/video25` 提供 640×360 RGB24、30 fps，前 15 个样本用于曝光预热。
- `CAMERA_COLOR_ORDER=BGR`：这块板的 RGB24 采集口虽然声明 RGB，实际字节顺序为 BGR。RGA 按 BGR_888 解读源 buffer，检测预处理与干净截图在原有遍历中转为真正 RGB。其他输出标准 RGB 的设备可设为 `RGB`；模型输入及文件回放的 RGB 契约不变。
- `CAMERA_ROTATION=180`：修正摄像头画面上下、左右同时倒置，检测与区域坐标系仍为 640×360。模型输入继续使用 RGB 640×640、114 灰边 letterbox，输出归一化 xyxy。
- `SCREEN_ROTATION=270`：文字、三区、框和摄像头画面一起逆时针转 90°，逻辑横屏为 800×480；原生屏幕仍为 `SCREEN_WIDTH=480`、`SCREEN_HEIGHT=800`，connector 97、plane 75。画面在逻辑界面内为 800×450，上下各 15 像素黑边，再整体映射到面板。不要为横屏直接交换原生面板尺寸。
- 两种旋转均以顺时针角度配置，支持 0/90/180/270。屏幕旋转只改变显示，独立相机旋转改变检测及标定坐标；修改相机旋转/采集尺寸时须使用对应 profile，手机视频 profile 不直接用于板载摄像头。

RGA 使用相机和屏幕的合成旋转角，CPU 图层通过坐标步长直接写入旋转后的 DMA-BUF，同一帧仍只需一次 RGA 缩放/旋转，没有新增整帧 CPU 旋转或中间画布复制。

旧版推理结束后才显示该帧，预览被约 7 FPS 的推理循环限制。新版采集使用 V4L2 DMA-BUF export，独立线程将当前帧交给 RGA 旋转/缩放到 DMA-BUF 输出池，再由 KMS 导入显示；CPU 在输出上写调试图层，预览不再整帧复制到普通内存。预览已实测约 29 FPS。

这是预览路径的 DMA 优化。检测分支仍映射采集输入、在 CPU 做色序转换及旋转/letterbox，并通过 `rknn_inputs_set` 交给 RKNN，尚未实现端到端零拷贝。YOLO26l 双输出模型单次 RKNN 阶段约 120 ms、检测约 7.6 FPS；轻量 YOLOv8n 六头 INT8 对照约 16.5 ms、检测约 29.9 FPS，两者的检测质量仍需按相同柜口素材比较。

appsink 与预览 appsrc 均为有界最新帧队列，输出池最多 4 个 buffer；实时负载过高时丢弃旧帧。`frame_index` 是 appsink 入队前的采集序号，`pts_ms` 是摄像头源 PTS，文件回放仍处理全部输入帧。事件只使用实际检测过的帧及源 PTS，不从 30 FPS 预览补造观测。

## 屏幕字段

| 显示 | 含义 |
| --- | --- |
| `DEBUG` / `LIVE` | 未标定示例 / 已人工确认的实时区域配置 |
| `TAKE` / `PUT` | 累计取出 / 放入；DEBUG 时是示例区域调试计数 |
| `UNCERTAIN` / `PENDING` | 不确定事件数 / 当前待完成穿越的轨迹数 |
| `VIEW` / `AI ... FPS` | 预览帧率 / 检测结果更新帧率 |
| `NPU ... MS` / `AI AGE ... MS` | 推理耗时 / 最新检测与当前预览的源 PTS 差 |
| `IN` / `TRANS` / `OUT` | 1 柜内 / 2 柜口过渡区 / 3 柜外的多边形边界 |
| `ID`、区域、`PENDING` | 跟踪 ID、目标当前区域与待穿越状态；中心点是区域判定锚点 |
| 底部文字 | 最近取放事件，或不确定原因，例如 `observation_gap_exceeded` |

绿色为 helmet，青色为 head；仅 helmet 参与取放计数。未成功建轨的检测显示 `UNTRACKED`。区域边界始终绘出并不表示事件触发，须观察到同一安全帽从一侧经过过渡区到另一侧。框/ID 来自最新完成的检测，可能落后于当前画面；`AI AGE > 400 ms` 时隐藏框。初次快照只有一个检测结果时，AI FPS 为 0 是正常的。

## 启动、停止及输出

以下命令在仓库 `custom_part` 目录执行，RUN_NAME 必须未使用过。默认模型为 `runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn`。

```sh
# 默认以 DEBUG 示例三区持续预览与检测。
flow_demo/board/run_rknn_camera_demo.sh start camera-debug-001
flow_demo/board/run_rknn_camera_demo.sh status camera-debug-001

# SIGTERM 正常停止、释放摄像头/DRM、拉回并校验输出。
flow_demo/board/run_rknn_camera_demo.sh stop camera-debug-001

# 自动处理 60 个检测帧后退出；结束后仍用 stop 收集。
MAX_FRAMES=60 flow_demo/board/run_rknn_camera_demo.sh start camera-smoke-001
```

配置可编辑 `board/camera.env`，或使用 `CONFIG=/path/to/camera.env`；环境变量覆盖默认值。实际配置、zones 副本、展开后的 `run.sh` 和模型/程序 SHA-256 保存在 `runs/board-camera/RUN_NAME/`。改变配置后停止旧 run，再以新名字启动。脚本会拒绝占用的摄像头/DRM，不自动关闭其他服务。前台运行支持 SIGINT/SIGTERM；`--no-display` 可仅验证采集、推理和事件。

### 替换已有 RKNN 模型

通过 `MODEL` 指定文件；程序查询输入输出，严格匹配支持的结构后再启动摄像头。`board/camera.env` 也可持久设置 `MODEL`，建议使用完整路径；留空仍使用原 YOLO26。

| `runs/rknn/` 中的模型 | 实时入口支持 | 用途 |
| --- | --- | --- |
| `helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn` | 是，六头 DFL=16 + NMS | 本次首选轻量候选，仅 `helmet` |
| `helmet-yolov8n-heads-int8/`、`helmet-yolov8n-heads-int8-test/` 中的 RKNN | 是，按各模型查询参数解码 | 历史量化候选，非本次摄像头实测版本 |
| `comp-2-freeze10-int8-20261111/split/` 中的 RKNN | 是，双输出 TopK | 原默认 `helmet/head` 模型 |
| `helmet-yolov8n-fp16-tk250/`、`helmet-yolov8n-heads-fp16/` | 否 | FP16 结构尚未接入实时后处理 |
| `comp-2-freeze10-int8-20261111/full/` | 否 | 已知混合量纲 INT8 分数归零的失败版本 |

```sh
# 先停止当前 run；RUN_NAME 使用未用过的新名字。
MODEL=flow_demo/runs/rknn/helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn \
  flow_demo/board/run_rknn_camera_demo.sh start camera-yolov8-001

# 停止后可换回原模型。
flow_demo/board/run_rknn_camera_demo.sh stop camera-yolov8-001
MODEL=flow_demo/runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn \
  flow_demo/board/run_rknn_camera_demo.sh start camera-yolo26-002
```

六头路径不使用历史常量：逐张量校验节点名、NCHW 尺寸、INT8/affine 类型及字节数，读取各自零点/尺度，再做 DFL、IoU=0.7 的 NMS 及 letterbox 坐标还原。两种模型复用相同检测/事件格式、BGR 校正、camera=180、screen=270 和 profile。轻量模型只有 helmet 类，屏幕 HEAD 数保持 0。

模型按 SHA-256 缓存在板端 `/userdata/flow_demo/models/`，以板端可用的 MD5 校验传输及复用内容；每次运行目录用符号链接引用该模型，避免重复副本占满 `/tmp`。实际缓存路径写在本机 run 的 `remote-model.txt`。旧模型和历史结果保留，缓存校验失败会退出。

新 run 的板端目录为 `/userdata/flow_demo/board-camera/RUN_NAME/`，实际路径记在本机 `remote-run.txt`；旧 run 的 `/tmp/flow_demo/board-camera/RUN_NAME/` 仍兼容。切换原因是 `/tmp` 已只余约 3 MB；历史产物保留，新结果不再占用 tmpfs。`stop` 拉回并核验：

- `console.log`、`exit_status`、`stats.json`：退出状态、采集/跳帧数、预览与检测 FPS、KMS 渲染数、DMA 输入/输出帧数。
- `detections.jsonl` 与 `tracks.jsonl`：逐检测帧的源序号、PTS、框、ID、区域、pending，两份帧信息必须对齐。
- `events.jsonl`：立即写出的事件日志。同方向多目标合并时，同一 `event_id` 可有多条 `op: "upsert"`；`count` 是更新后总数，累加时使用 `delta_count`，或按 ID 保存最后一条，不能直接累加所有 count。
- `summary.json`：退出时写出的正式/DEBUG 取放数、不确定数、处理到的源帧与 PTS。运行异常时 `run_ok=false`，没有处理帧或使用 DEBUG 区域时 `result_complete=false`。该字段只描述会话判定完整性，不代表准确率验收。
- `preview.ppm`：预热后首张包含检测状态的屏幕画面；`source.ppm`：首个检测帧的干净旋转画面，可用于标定。两者都只保存一次。

新统计字段 `avg_preview_ms` 是 RGA、图层和 appsrc 提交的平均耗时，`avg_arrival_to_result_ms` 是采集入队到检测结果就绪的平均时间，均不等于传感器至面板的实际延迟。`camera_rotation`、`screen_rotation` 和原生 `screen_width/height` 分别记录方向及面板尺寸，`capture_caps_format` 与 `capture_color_order` 分别记录声明格式与实际字节顺序。早期测试文件保留原字段，不改写历史产物。旧 run 的板端 `/tmp` 重启后可能丢失，测试结束应收集结果。

## 标定实际柜口

默认 `board/camera-zones-debug.json` 的 inside/transition/outside 是画面从左到右的三条示例区域，`geometry_confirmed=false`。脚本默认显式带 `--debug-zones`，屏幕标记 DEBUG，事件标记 `status: "debug"`，摘要的正式计数保持为 0。没有标定时，这些值只用于观察程序链路。

安装好摄像头后，先短测并收集干净 `source.ppm`，在 PC 点击实际柜内、柜口、柜外区域：

```sh
flow_demo/.venv-rknn310/bin/python flow_demo/board/calibrate_camera_zones.py \
  --images flow_demo/runs/board-camera/camera-smoke-001/source.ppm \
  --output flow_demo/tracking/config/local/rv1126b-camera.json \
  --rotation 180
```

按 `1/2/3` 选择柜内/过渡区/柜外，点击描多边形；`u` 撤销点，`r` 重画当前区域，`[` / `]` 切换多张样本，检查后按 `y` 保存。`--images` 可提供多张相同旋转尺寸的干净截图，覆盖常见操作位置。输出包含截图哈希、旋转、尺寸及人工确认状态，已有文件不会被覆盖。

```sh
ZONES_PROFILE=flow_demo/tracking/config/local/rv1126b-camera.json DEBUG_ZONES=0 \
  flow_demo/board/run_rknn_camera_demo.sh start camera-calibrated-001
```

此时屏幕显示 LIVE，事件可标记 confirmed。改变安装位置、裁剪或旋转后重新标定；仅物体移动无需重画。默认观测缺口上限为 400 ms；长遮挡、断检或 ID 更换不能靠库存差补算取放。保持目前 tracker 阈值，不为了出现事件而自动放宽建轨条件。

## 与原有服务切换

用户已授权停止占屏的 `media_engine`，本次通过 SIGTERM 正常退出并释放设备，没有修改开机自启动。SIGSTOP 保留设备 FD，不适合释放摄像头和屏幕。停止 demo 后，如需恢复原服务，可单独执行：

```sh
adb -P 5038 -s 192.168.1.63:5555 shell '
export LD_LIBRARY_PATH=/oem/usr/lib:/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_PATH=/oem/usr/lib/gstreamer-1.0
export GST_PLUGIN_SCANNER=/oem/usr/libexec/gstreamer-1.0/gst-plugin-scanner
nohup /oem/usr/bin/media_engine --config /oem/usr/share/media_engine.yaml \
  >/tmp/media_engine-flow-restore.log 2>&1 </dev/null &
sleep 1
'
```

不直接调用 `S60gb28181 start`，因为它还会启动 GB28181 daemon。停止 demo 不自动恢复原服务。

## 2026-11-13 验证

目标板为 `Rockchip RV1126B XIAOYU 50IPC V10 Board`，RKNN API 2.3.2、驱动 0.9.8；模型 SHA-256 为 `1ca2ad5809b1add183fc33f7d09e9dc19b6d8492b10d7e63b68320c45a3bd92c`。

| 指标 | 旧版 `live-20261113-soak` | 新版短测 `live-20261113-dma-events02` | 新版长测 `live-20261113-events-soak02` |
| --- | ---: | ---: | ---: |
| 检测帧数 | 1501 | 60 | 1057 |
| 预览 FPS | 7.086 | 29.251 | 29.965 |
| 检测 FPS | 7.086 | 7.512 | 7.533 |
| KMS 实际渲染帧数 | 1501 | 235 | 4206 |
| DMA 输入 / 输出帧数 | 未统计 | 235 / 235 | 4206 / 4206 |
| 平均 RKNN 推理 | 117.748 ms | 117.975 ms | 118.311 ms |
| 平均预处理 | 6.523 ms | 9.812 ms | 9.349 ms |
| 平均预览处理 | 11.995 ms（旧同帧绘制） | 5.219 ms | 4.334 ms |

`board/tests/check_preview_rate.py` 以实际 KMS 渲染量校正预览帧率，旧版在 ≥24 FPS 检查中失败，新版通过。新版 60 帧检测与轨迹的源 PTS/序号对齐，235 个提交的 DMA 输出全部被 KMS 渲染；退出正常，截图比例、方向、三区和文字已回读查看。

新版长测约 140 秒，采集/预热/检测/跳过为 4226/15/1057/3154，4206 个预览 DMA 输出全部渲染，平均采集入队到 AI 结果就绪 147.736 ms。SIGTERM 正常退出，检测/轨迹与会话末帧 PTS 对齐，事件日志与摘要计数一致，停止后 `/dev/video25`、`/dev/dri/card0` 无残留 FD，`win0-0` 恢复 DISABLED。

上轮竖屏调试 run `live-20261113-events-display` 已正常停止并收集。其运行中读取 DRM framebuffer，480×800 的 3 个 `annotated-frame` 对象均为 `imported=yes`；相同大小的 3 个 system-uncached DMA-BUF 同时挂接 RGA 和 display-subsystem，确认实际使用 DMA 导入。证据在该 run 的 `drm-framebuffer.txt`、`dma-buf.txt` 和 `drm-summary.txt`。

方向调整后，`live-20261113-ccw90-smoke` 的 90 帧检测短测通过：camera=0、screen=270，采集/预热/检测/跳过为 357/15/90/252，337 个预览 DMA 输出全部渲染，预览 29.562 FPS、检测 7.943 FPS，平均预处理 1.219 ms。干净源图为 640×360，原生屏幕截图为 480×800，按横屏阅读后的截图为 800×480；场景对照确认 RGA 画面与图层坐标同向。四种整屏旋转及带 padding 的完整图层 PC 像素对照、ASan/UBSan、交叉编译和正常停止检查通过。

随后针对用户反馈的色差，使用同一个 AIQ 会话同时采集 selfpath RGB24 与 mainpath NV12，避免两次初始化的白平衡差异。NV12 声明 `colorimetry=1:4:16:4`，对应 full-range BT.601，按此转换后进行高斯平滑像素对照：

| 对照 | 原顺序 RGB 与 NV12 的 MAE | 交换红蓝后的 MAE | 判定 |
| --- | ---: | ---: | --- |
| 修正前，源 PTS 相差 1.714 ms | 4.206 | 0.326 | 声明色序错误，检查失败 |
| 修正后 C 预处理输出，源 PTS 相差 1.549 ms | 0.323 | 4.331 | RGB 色序检查通过 |

诊断程序与前后采集证据保存在 `runs/board-camera/diagnose-color-20261113/`，修正后复测在其 `green03/` 下。NV12 只作为诊断参考，正式预览保留已验证的 RGB24 DMA 口。上述误差验证红蓝通道顺序，不代表色彩标定或检测精度验收。

色序及 camera=180、screen=270 修正后的 `live-20261113-color180-smoke` 完成 90 帧检测，采集/预热/检测/跳过为 372/15/90/267。352 个预览 DMA 输出全部被 KMS 渲染，预览 29.585 FPS、检测 7.597 FPS，平均推理 119.796 ms、预处理 6.558 ms。`check_preview_rate.py` 的 ≥24 FPS 检查、检测/轨迹帧对齐、事件日志/摘要一致性及正常退出均通过。回读干净源图和横屏截图，人物方向朝上，RGA 显示与检测输入颜色一致；这次测试没有真实柜口取放动作。RGB/BGR 在四种相机旋转、带 padding 的 stride 及缩放路径的像素对照、ASan/UBSan 和 `-Wall -Wextra -Werror` 交叉编译通过。

`live-20261113-color180-display` 已正常停止并收集，累计 12709 帧检测、预览 29.996 FPS、检测 7.570 FPS，RKNN 阶段平均 120.270 ms。其运行中回读 DRM framebuffer，3 个 480×800 的 `annotated-frame` 均为 `imported=yes`，证据为该 run 的 `drm-framebuffer.txt`；初始化日志节选保存在 `launch-check.txt`。

### 六头轻量模型对照

`helmet-yolov8n-heads-int8-eval` 的 SHA-256 为 `c06667175be8bc79f1cd6ed903b5cc38fcc7c05053d1f094f83218d999af1b04`，文件约 4.19 MB，原 YOLO26l 双输出约 28.84 MB。`model-swap-20261113-parity` 的三帧板端同输入回归确认：新程序的 YOLO26 JSONL 与旧程序逐字节一致；六头原生 INT8 dump 经独立 NumPy DFL/NMS 解码及坐标还原，与新程序结果一致。为获得非空数值对照，该检查单独使用 conf=0.005；实时默认仍为 0.25。PC 11 项新适配测试及相关后处理合计 18 项通过，覆盖动态量化参数、横竖源坐标、NMS、真实板端张量、空帧和错误结构拒绝；真实六头输出的 ASan/UBSan 检查通过。

`live-20261113-yolov8-int8-smoke` 在相同相机、方向和色序配置下完成 900 帧检测：

| 指标 | YOLO26l（前一持续运行） | YOLOv8n（900 帧测试） |
| --- | ---: | ---: |
| 检测 FPS | 7.570 | 29.881 |
| 预览 FPS | 29.996 | 29.869 |
| 平均 RKNN 阶段 | 120.270 ms | 16.543 ms |
| 平均预处理 | 6.238 ms | 6.227 ms |
| 平均后处理 | 4.643 ms | 0.330 ms |
| 平均入队至检测结果就绪 | 151.454 ms | 23.983 ms |

采集/预热/检测/跳过为 920/15/900/5，901 个 DMA 预览输出全部渲染；≥24 FPS 检查、检测/轨迹 PTS 和帧号对齐、事件日志/摘要计数及正常停止检查通过。该测试约 30 秒，当前场景没有实际安全帽取放，后处理耗时不代表多目标密集场景。

速度提升不代表柜口效果通过：同一组三张柜口样本在 conf=0.25 时，YOLO26 检出数量为 0/0/1，YOLOv8n 为 0/0/0；样本不足以计算精度指标，但已提示模型差异，需要真实安全帽和相同柜口素材对照。YOLOv8n 只输出 helmet，不检测 head。当前仍使用未标定的 DEBUG 示例区域。

当前保留的屏显对照 run 为 `live-20261113-yolov8-int8-display`，板端输出在 `/userdata/flow_demo/board-camera/`；脚本默认模型仍是原 YOLO26。状态及停止命令：

```sh
flow_demo/board/run_rknn_camera_demo.sh status live-20261113-yolov8-int8-display
flow_demo/board/run_rknn_camera_demo.sh stop live-20261113-yolov8-int8-display
```

`preview.ppm` 是原生面板方向，`preview-landscape.png` 是旋转为横屏阅读方向的同一截图；`source.ppm`/`source.png` 是干净标定截图，标定参数使用 `--rotation 180`。更早 run 中的截图仍使用它们各自记录的旋转配置。

PC 测试覆盖 tracker、CrossingEngine、CLI 与实时 C ABI 共 4 项；实时 adapter 的往返取放、DEBUG 不产生正式计数、双目标合并、旋转不匹配、未确认区域拒绝及观测缺口均有验证，同一事件核心也已在 RV1126B 执行。图像测试覆盖旋转、stride、letterbox、屏幕映射与过期框隐藏，并通过 ASan/UBSan；交叉编译启用 `-Wall -Wextra -Werror`。

最终事件适配测试还覆盖异常退出与空会话的完整性标记，板端证据为 `runs/board-camera/live-events-core-20261113-final/`。完整新程序的板端文件输入回归与旧版检测 JSONL 逐字节一致，仍为 1 个 helmet，证据为 `runs/board-camera/file-regression-events-20261113/`。

当前摄像头场景主要是天花板，尚未在真实柜口验证安全帽取放。合成轨迹事件测试和高帧率预览证明链路可运行，不构成真实动作准确率验收；既有手机视频动作验收仍未通过，见 `board-video-demo.md`。
