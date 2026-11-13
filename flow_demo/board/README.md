# RKNN 板端工具

板载摄像头入口 `out/rknn_yolo26_infer --camera` 已接入 BoT-SORT 与取放事件，屏幕显示三区、ID、待穿越状态、取放/不确定数及预览/检测 FPS。DMA-BUF/RGA/KMS 预览约 30 FPS，检测约 7.6 FPS；检测输入仍使用 CPU letterbox 和 RKNN 普通输入接口。默认 `/dev/video25`、`CAMERA_COLOR_ORDER=BGR`、相机旋转 180°、整屏旋转 270°（逆时针 90°），原生面板 480×800、逻辑横屏 800×480。初始化参照 `media_engine`，BGR 配置适配此板 RGB24 口的实际字节顺序，模型输入仍为真正 RGB。示例三区仍为未确认的 DEBUG 配置；在 `camera.env` 设置 `ZONES_PROFILE`，或用 `calibrate_camera_zones.py` 标定实际机位。通过 `run_rknn_camera_demo.sh start|status|stop RUN_NAME` 管理。完整参数和验证见 [摄像头屏显说明](../docs/board-camera-demo.md)。

实时/文件入口现支持自动选择 YOLO26 双输出 INT8 与本项目单类别 YOLOv8 六头 INT8 后处理，严格查询并校验张量，六头使用各自量化参数做 DFL/NMS。通过 `MODEL` 环境变量或 `camera.env` 可切换；默认保留原 YOLO26。候选 `runs/rknn/helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn` 的 900 帧摄像头测试检测 29.88 FPS、预览 29.87 FPS，RKNN 阶段 16.54 ms；只检测 helmet。三张柜口样本在 conf=0.25 下该候选未检出，速度结果不能替代场景效果评估。FP16 和完整图混合 INT8 未接入此入口；详细兼容表与切换命令见摄像头说明。

这个目录是 `flow_demo` 的独立板端验证工具，不接入 `media_engine` 生产目标。它验证 RKNN 运行时能否在 RV1126B 上加载模型、查询输入输出属性、重复推理并释放输出；`RKNN_SMOKE_OK` 只表示运行时门禁通过，不表示检测精度或动作事件已经通过。

指定候选 `comp-2-freeze10.onnx` 的输出与下文旧 YOLOv8n DFL 六头不同；使用新增 `out/rknn_yolo26_postprocess` 读取两个独立 INT8 输出，并通过参数传入当前模型查询到的零点/尺度。该 C 程序已在 RV1126B 执行，复现原 ONNX 的两阶段 TopK(300)，不做 DFL 或 NMS。新模型的转换、实测退化及命令见 [专项记录](../docs/comp-2-freeze10-rknn.md)。`make` 会同时编译此工具；旧模型工具保留。

`make` 还会构建 `out/rknn_yolo26_infer`：它读取连续的 640×640 RGB 帧流和源帧元数据，查询当前 RKNN 的双输出属性，在板端运行推理、INT8 反量化、两阶段 TopK(300) 和 letterbox 坐标还原，输出 `tracking/flow_demo_cli` 可直接读取的归一化检测 JSONL。它会校验输出节点名、形状、类型和量化参数，不把 smoke 默认值当作模型属性。

## 视频帧回放入口

先由 PC 用 OpenCV/ffprobe 解码视频并按 640×640、RGB、padding 114、NHWC 写出逐帧输入及源 PTS/letterbox 元数据；随后把帧流推到 RV1126B，由 RKNN 在板端做推理和后处理。若传入已经经过人工机位审查的区域配置，板端还会运行 BoT-SORT/区域穿越并把轨迹、事件和摘要拉回 PC：

```sh
make -C board
tracking/build_rv1126b.sh
ADB_SERVER_PORT=5038 ADB_SERIAL=192.168.1.63:5555 MAX_FRAMES=3 CONF=0.001 \
  board/run_rknn_video_demo.sh 'dataset/raw_videos/单项取出.mp4' detector-smoke
```

默认只运行检测，不需要区域配置。要生成动作事件时，把经人工确认且 `geometry_confirmed=true` 的 zones JSON 作为第三个参数传入；不满足条件时脚本拒绝运行事件流程。默认模型是 `runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn`，默认设备为 `192.168.1.63:5555`，置信度为 0.25、最多处理 30 帧；设置 `MAX_FRAMES=0` 才处理整个视频。帧流按 `BATCH_FRAMES`（默认 30）分块推到板端，逐块推理后在 PC 顺序合并检测 JSONL，避免向板端一次传整个 RGB 文件。可用 `MODEL`、`CONF`、`MAX_FRAMES`、`BATCH_FRAMES`、`ADB_SERIAL`、`ADB_SERVER_PORT` 和 `OUTPUT_ROOT` 覆盖。每次运行写入新的 `runs/board-video/<RUN_NAME>/`，不会覆盖已有结果，并保存模型/输入哈希、板型和板端 runtime 日志。

脚本在视频预处理之前查询实际板型，默认预检超时为 15 秒（ADB_PREFLIGHT_TIMEOUT）；每次 ADB 调用默认最多等待 120 秒（ADB_TIMEOUT），均可通过环境变量调整为正整数秒。主机需提供 timeout 命令。连接离线、shell 无响应或板型不是 RV1126B 时会退出，并在新 run 中保留 adb-preflight.log。恢复设备后请使用新的 RUN_NAME 重试；脚本不会自动重启板端。

柜内深处左右两角的画面可分别使用各自的 zones JSON，不要求把相机位置限制在单侧。当前脚本按单段视频/单个机位运行；配置坐标始终对应该段视频的画面。

```sh
board/run_rknn_video_demo.sh 'left-view.mp4' left-view-run zones-left.json
board/run_rknn_video_demo.sh 'right-view.mp4' right-view-run zones-right.json
```

左右 zones 都要在各自视角上人工描出 inside、transition、outside 区域后，才把 `geometry_confirmed` 设为 `true`。当前流程分别产出各自的事件 JSONL，不把两侧轨迹自动配对或合并。

## 动态机位区域

用交互标定器生成不写死在程序中的机位 profile：

```sh
.venv-rknn310/bin/python tracking/calibrate_zones.py --video dataset/raw_videos/单项取出.mp4 --camera-id left-corner --output tracking/config/local/left-corner.json --display-scale 0.7
```

确认九张采样帧后才会保存 profile；右侧深角相机需单独标定。运行脚本会为每段视频生成当次 run 的 zones.json，填入实际 video 和 session_id。同一固定机位的不同视频可复用 profile；机位或裁剪变化后重新标定。

当前入口的解码和预处理仍在 PC；NPU 推理、后处理及可选跟踪/事件在 RV1126B。它是板端模型与跟踪联调通道，不等同于 RV1126B 本机 GStreamer 解码、实时摄像头或检测精度验收。

本次三帧板端联调记录见 [`docs/board-video-demo.md`](../docs/board-video-demo.md)。

## 交叉编译

在当前 SDK 的 `custom_part` 工作树中执行：

```sh
cd flow_demo/board
make
```

也可以显式指定 SDK 路径、头文件和运行库：

```sh
make SDK_ROOT=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
```

产物包括 `board/out/rknn_smoke`（调用 RKNN 的 AArch64 smoke 程序）和 `board/out/rknn_int8_postprocess`（不依赖 RKNN 的 AArch64 INT8 六头后处理程序）。构建 smoke 使用 SDK 的 `rknn_api.h` 和 `output/out/media_out/lib/librknnrt.so`；板端必须提供与固件匹配的 `librknnrt.so` 和 NPU 驱动。后处理程序可单独用本机编译器构建：

```sh
make CC=gcc POSTPROCESS_TARGET=out/rknn_int8_postprocess-host out/rknn_int8_postprocess-host
```

## 准备输入

RKNN 转换记录的输入契约是 640×640、NHWC、RGB、padding 114。用训练环境的 Pillow 将一张样本图变成无编码的 RGB 输入：

```sh
python training/prepare_rknn_input.py \
  --image runs/predict/helmet-sample-20261111-020041/005308_jpg.rf.7faf545c7704680ac6a605a478c8c6a6.jpg \
  --output /tmp/flow_demo/sample.rgb
```

命令同时生成 `/tmp/flow_demo/sample.rgb.json`，其中包含原始输入 SHA-256、缩放和 padding 参数，后续用于输入身份核验与检测框坐标还原。

## 先验证 FP16，再验证 INT8

第一轮建议使用单输出 FP16 模型，减少后处理变量：

```sh
adb push board/out/rknn_smoke /tmp/flow_demo/
adb push /tmp/flow_demo/sample.rgb /tmp/flow_demo/
adb push runs/rknn/helmet-yolov8n-fp16-tk250/helmet-yolov8n-rv1126b-fp16.rknn \
  /tmp/flow_demo/
adb shell 'chmod +x /tmp/flow_demo/rknn_smoke && \
  LD_LIBRARY_PATH=/oem/usr/lib /tmp/flow_demo/rknn_smoke \
  --model /tmp/flow_demo/helmet-yolov8n-rv1126b-fp16.rknn \
  --input /tmp/flow_demo/sample.rgb --runs 5'
```

如果使用 `scp`，把上述三个文件复制到板端同一目录，再执行：

```sh
MODEL=/tmp/flow_demo/helmet-yolov8n-rv1126b-fp16.rknn \
INPUT=/tmp/flow_demo/sample.rgb \
RKNN_LIB_DIR=/oem/usr/lib \
./board/run_rknn_smoke.sh
```

FP16 预期 `outputs=1`。INT8 六头模型可以用同一工具验证运行时，但预期 `outputs=6`，并且输出属性应显示 `int8` 与非零 `scale`/`zp`；在完成反量化、DFL 解码和 NMS 前，不要把它接入跟踪器：

```sh
MODEL=/tmp/flow_demo/helmet-yolov8n-rv1126b-int8.rknn \
INPUT=/tmp/flow_demo/sample.rgb \
./board/run_rknn_smoke.sh
```

需要把板端原生 INT8 输出带回 PC 做后处理对照时，直接调用可执行文件并关闭 `want_float`；`--dump-dir` 会写出 `output-0.bin` 到 `output-5.bin`：

```sh
adb -s 192.168.1.63:5555 shell \
  'LD_LIBRARY_PATH=/oem/usr/lib /tmp/flow_demo/rknn_smoke \
   --model /tmp/flow_demo/helmet-yolov8n-rv1126b-int8.rknn \
   --input /tmp/flow_demo/sample.rgb --runs 1 --want-float 0 \
   --dump-dir /tmp/flow_demo/dump-int8'
adb -s 192.168.1.63:5555 pull /tmp/flow_demo/dump-int8 flow_demo/results/rknn-dump-20261111
```

C 后处理工具也可以直接解析这些原始文件，在板端输出 JSON 检测框；使用较高阈值查看候选框：

```sh
./board/out/rknn_int8_postprocess \
  --dump-dir /tmp/flow_demo/dump-int8 --conf 0.25 --iou 0.7 --max-det 300
```

它与 Python 参考实现共用相同的量化参数和解码约定。PC 端对已保存板端 dump 的逐框比较通过；交叉编译已通过，当前还未在 RV1126B 上执行此 C 后处理程序。

板端记录至少保留完整 stdout/stderr、`uname -a`、RKNN API/driver 版本、模型 SHA-256、模型转换目录中的 `conversion.json` 和每轮耗时。若输入尺寸、布局、类型或输出数量与转换元数据不一致，先停在 smoke 门禁，不要用代码里的默认值掩盖差异。

本次真板记录保存在 `results/rknn_smoke-20261111-board.log`，汇总与后续门禁见 `docs/board-rknn-smoke.md`。
