# 训练代码

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

## 数据使用边界

该 ZIP 的声明许可证为 CC BY 4.0，来源信息保存在生成的 `conversion.json` 中。它是通用 PPE 数据，不能替代柜内实际场景素材。后续应将自己的柜口视频抽帧并人工标注，再按完整视频或会话划分数据，避免相邻帧泄漏到验证集和测试集。
