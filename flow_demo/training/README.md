# 训练代码

`prepare_rknn_video.py` 可将原视频逐帧解码为 RV1126B demo 接收的 RGB 输入帧流，并为每帧保留 `ffprobe` 源 PTS、原始尺寸及 letterbox 参数。板端推理和跟踪入口见 `../board/README.md`。

视频读取会先应用 MP4 容器中的显示方向元数据（例如手机写入的 180° `Display Matrix`），因此预处理、标定和预览使用同一显示方向；不要对带旋转元数据的视频再手工旋转。

当前训练目标是单类别 `helmet` 检测。`dataset/PPE.zip` 是 Roboflow YOLOv8 数据集，原始类别为 `Gloves`、`Vest`、`goggles`、`helmet`、`mask`、`safety_shoe`；准备脚本只保留 `helmet`，并将它重映射为 class `0`。该数据集部分标签实际是 YOLO 分割多边形，脚本会把目标多边形转换为外接检测框。

## 在训练机器上准备环境

使用 Python 3.10–3.12。在使用 NVIDIA GPU 的机器上，先按该机器的 CUDA 版本安装匹配的 PyTorch，再安装项目训练依赖：

```bash
cd /path/to/flow_demo
python3 -m venv .venv
. .venv/bin/activate
python -m pip install --upgrade pip
python -m pip install -r training/requirements.txt
```

CPU 也可以运行准备脚本和小规模验证；正式训练建议使用 CUDA GPU。训练脚本不依赖当前机器的绝对路径，数据 YAML 生成后可随 `dataset/prepared/ppe_helmet/` 一起复制到另一台机器。

## 检查并转换数据集

不写文件地检查 ZIP 的类别和标注统计：

```bash
python training/prepare_dataset.py --archive dataset/PPE.zip --check-only
```

生成可直接给 Ultralytics 使用的单类别数据集：

```bash
python training/prepare_dataset.py \
  --archive dataset/PPE.zip \
  --output dataset/prepared/ppe_helmet
```

生成目录包含 `dataset.yaml`、`conversion.json`、`images/` 和 `labels/`。原始 ZIP 不会被修改；如果需要重新生成已有目录，显式添加 `--force`。

## 训练 YOLOv8n

默认使用 `yolov8n.pt`、640 输入尺寸和自动 batch。首次运行会从 Ultralytics 下载预训练权重；也可以把 `--model` 换成本地 `.pt` 路径。

```bash
python training/train.py \
  --data dataset/prepared/ppe_helmet/dataset.yaml \
  --model yolov8n.pt \
  --device 0 \
  --epochs 100 \
  --name helmet-yolov8n
```

CPU 示例：

```bash
python training/train.py \
  --data dataset/prepared/ppe_helmet/dataset.yaml \
  --device cpu \
  --batch 4 \
  --workers 2 \
  --no-amp \
  --epochs 2 \
  --name helmet-cpu-smoke
```

正式结果通常位于 `runs/train/<name>/weights/best.pt`。训练脚本会额外保存 `flow_demo_train_config.json`，记录实际参数。

## 导出 ONNX

```bash
python training/export_onnx.py \
  --weights runs/train/helmet-yolov8n/weights/best.pt \
  --output models/helmet-yolov8n.onnx \
  --imgsz 640 \
  --opset 12 \
  --device cpu
```

导出会检查 checkpoint 是否只有 `helmet` 类，并校验 ONNX 模型及其静态 `float32 [1,3,640,640]` NCHW 输入。若有意覆盖已有输出，添加 `--force`。这个文件还要经过独立的 ONNX smoke、RKNN 转换、INT8 校准和 RV1126B 板端验证；PC 训练成功不代表板端模型已经可用。

## 解码 RV1126B INT8 六头输出

`postprocess_rknn_int8.py` 读取板端 `want_float=0` 保存的 `output-0.bin` 至 `output-5.bin`，使用板端查询到的六组量化参数做反量化，再执行 DFL 解码和单类别 NMS。默认参数与保存的通用 PPE 评测一致（置信度 `0.001`、IoU `0.7`、最多 300 框）：

```bash
python training/postprocess_rknn_int8.py \
  --dump-dir results/rknn-dump-20261111/dump-int8 \
  --output results/rknn-dump-20261111/detections-int8.json
```

要映射到原始图片坐标，传入生成该 RGB 输入时的 `.rgb.json` sidecar：

```bash
python training/postprocess_rknn_int8.py \
  --dump-dir results/rknn-dump-20261111/dump-int8 \
  --manifest /tmp/flow_demo/sample.rgb.json \
  --conf 0.25
```

评测阈值 `0.001` 会保留低置信候选，并可能达到 300 框上限；查看检测结果时可用更高 `--conf`。板端 dump 如未同时保存输入 RGB 的哈希和 sidecar，就不能据此声称已完成 PC/板端同图坐标对照。

## 数据使用边界

该 ZIP 的声明许可证为 CC BY 4.0，来源信息保存在生成的 `conversion.json` 中。它是通用 PPE 数据，不能替代柜内实际场景素材。后续应将自己的柜口视频抽帧并人工标注，再按完整视频或会话划分数据，避免相邻帧泄漏到验证集和测试集。

## 指定 ONNX 的 RV1126B INT8 量化

`comp-2-freeze10.onnx` 为 YOLO26l 两类模型（`helmet=0`、`head=1`），与已有 YOLOv8n 单类模型的输出契约不同。运行记录与结果见 `../docs/comp-2-freeze10-rknn.md`。从 `flow_demo/` 执行：

```sh
# 先安装 CPU PyTorch 2.4.0、requirements-rknn.txt，
# 再以 --no-deps 安装适合当前 Python ABI 的本地 RKNN Toolkit2 2.5.0 wheel。
RUN=runs/rknn/comp-2-freeze10-int8-new
python training/prepare_rknn_calibration.py \
  --dataset dataset --videos dataset/raw_videos --output-dir "$RUN/calibration"
python training/convert_rknn.py \
  --onnx comp-2-freeze10.onnx --dataset "$RUN/calibration.txt" \
  --output-dir "$RUN/split" --yolo26-split
```

校准集默认从 PPE `train` 随机选 100 张图、从 11 段柜口视频均匀取 100 帧，seed 为 0；不从 PPE `valid/test` 抽校准图。视频抽帧用作校准后，不能将这些样本再称为独立精度 holdout。原图/视频与处理后 PNG 的哈希、抽帧位置、letterbox 参数都记录在 run 内。工具拒绝覆盖已有转换目录和校准清单。

必须使用 `--yolo26-split` 分开概率 `[1,2,8400]` 与已解码坐标 `[1,4,8400]`，让两者分别量化；完整图 `[1,300,6]` 将大坐标与小分数混合量化，真板已实测出现分数/类别归零。两输出通过 `board/rknn_yolo26_postprocess` 或 `training/compare_yolo26_board.py` 反量化后，复现原图的两阶段 TopK(300)，不再加 sigmoid、DFL 或 NMS。C 程序的零点与尺度必须取自当前模型的板端属性查询。
