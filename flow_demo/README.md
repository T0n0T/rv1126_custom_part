# flow_demo

面向 RV1126B 售卖柜的安全帽出入检测 Demo：检测安全帽本体，通过 BoT-SORT 跟踪和柜内、柜口过渡区、柜外三区判定取出、放入或无法确认的过程。区域保存在独立 JSON profile 中，可按实际机位配置。

## 当前能力与边界

- 板载摄像头可在板端完成 INT8 检测、跟踪、区域事件记录及屏幕调试，显示框、ID、三区、取放计数和 AI 结果年龄。
- 预览使用 DMA-BUF → RGA → KMS，约 30 FPS；检测仍有 CPU 预处理和 RKNN 输入复制，尚非全链路零拷贝。
- 实时与文件输入共用检测和事件格式。文件联调目前由 PC 解码、分批发送 RGB，板端执行推理与跟踪；板端本机文件解码尚待验证。
- 支持本项目 YOLO26 双输出 INT8 和单类别 YOLOv8 六头 INT8，运行时校验张量结构并读取量化参数。YOLO26l 检测约 7.6 FPS，轻量 YOLOv8n 在当前摄像头测试场景约 30 FPS；速度和检测效果须分别评估。
- 默认区域是明确标记的 **DEBUG 示例**。实际板载机位仍需标定；真实柜口取放验收尚未通过，不能把运行成功、通用 PPE 指标或预览帧率当成动作准确率。

历次功能、修复、板端测试和已知问题记录在 [CHANGELOG.md](CHANGELOG.md)，详细证据见各专项文档。

## 摄像头快速开始

以下命令从 `flow_demo/` 执行。交叉编译依赖上层 RV1126B SDK，板端需提供匹配的 RKNN、AIQ、RGA 和 GStreamer 运行库。模型文件不随 Git 分发，先按下表准备所需本地文件；启动前正常停止占用摄像头和 DRM 的服务。

```sh
make -C board
board/run_rknn_camera_demo.sh start camera-debug-001
board/run_rknn_camera_demo.sh status camera-debug-001
board/run_rknn_camera_demo.sh stop camera-debug-001
```

每次使用未用过的运行名称。`stop` 会正常退出、释放设备并拉回结果。参数集中在 [board/camera.env](board/camera.env)，也可通过同名环境变量覆盖；默认目标为 `192.168.1.63:5555`、ADB server 端口 `5038`。此板当前使用 BGR 色序、相机旋转 180°、整屏旋转 270°，原生面板为 480×800。

| 模型 | 本地路径 | 说明 |
| --- | --- | --- |
| YOLO26l INT8，默认 | `runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn` | `helmet/head` 两类，概率与坐标独立量化 |
| YOLOv8n INT8，轻量候选 | `runs/rknn/helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn` | 单类别 `helmet`，六头 DFL/NMS，柜口效果待比较 |

停止旧运行后，可通过 `MODEL` 切换，也可在 `camera.env` 中持久设置模型的完整路径：

```sh
MODEL="$PWD/runs/rknn/helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn" \
  board/run_rknn_camera_demo.sh start camera-yolov8-001
```

FP16 模型尚未接入实时后处理；`comp-2-freeze10-int8-20261111/full/` 是已知分数量化归零的失败版本。完整兼容表、标定、输出格式及 `media_engine` 服务切换方式见 [摄像头屏显说明](docs/board-camera-demo.md)。

## 区域标定与事件

板载摄像头用 [board/calibrate_camera_zones.py](board/calibrate_camera_zones.py) 在干净 `source.ppm` 截图上标定；手机视频用 [tracking/calibrate_zones.py](tracking/calibrate_zones.py) 跨多张采样帧复核区域。不同机位分别保存 profile；固定机位可复用，视角、裁剪或相机旋转改变后需重新确认。区域坐标不写死在检测程序中。

仅 `helmet` 参与取放计数。事件使用实际检测帧的源 PTS；长遮挡、断检或无法连续关联的轨迹保留为不确定结果。DEBUG 区域不会产生正式确认计数。配置格式和使用方法见 [区域配置说明](tracking/config/README.md)。

## 开发与验证

PC 端跟踪与事件核心可独立构建测试：

```sh
cmake -S tracking -B tracking/build -DCMAKE_BUILD_TYPE=Debug
cmake --build tracking/build -j4
ctest --test-dir tracking/build --output-on-failure
make -C board test-image
```

Python 工具测试使用安装了相应依赖的环境运行：

```sh
python -m pytest -q training/tests tracking/tests
```

训练及导出见 [training/README.md](training/README.md)，RKNN 工具与交叉编译见 [board/README.md](board/README.md)。当前验证重点是实际机位标定、遮挡下的检测与轨迹连续性，以及带逐次动作真值的独立评测。

## 文档与版本管理

| 文档 | 内容 |
| --- | --- |
| [CHANGELOG.md](CHANGELOG.md) | 按日期记录功能、修复、验证结果和限制 |
| [docs/spec.md](docs/spec.md)、[docs/plan.md](docs/plan.md) | 需求、事件语义、实现边界与后续工作 |
| [docs/board-camera-demo.md](docs/board-camera-demo.md) | 实时摄像头、模型切换、屏显、标定与服务管理 |
| [docs/board-video-demo.md](docs/board-video-demo.md) | 视频分批回放、逐段结果及遮挡问题 |
| [docs/comp-2-freeze10-rknn.md](docs/comp-2-freeze10-rknn.md) | YOLO26 转换、双输出契约及量化退化 |
| [docs/board-rknn-smoke.md](docs/board-rknn-smoke.md) | RKNN runtime 与旧 YOLOv8 模型 smoke |
| [tracking/README.md](tracking/README.md) | BoT-SORT、区域事件 CLI 与标定工具 |
| [dataset/README.md](dataset/README.md) | 数据准备、标注和划分约定 |

Git 管理源码、脚本、测试、示例配置、文档及第三方源码与许可证。训练环境、原始视频与数据集、模型权重、`runs/`、`results/`、编译输出和 `tracking/config/local/` 下的实际机位 profile 均留在本地并由 `.gitignore` 排除。克隆仓库后，需另行准备模型、素材和设备配置。
