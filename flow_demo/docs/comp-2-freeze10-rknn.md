# comp-2-freeze10：RKNN INT8 转换与板端测试

## 2026-11-11 结论

已按指定使用 `flow_demo/comp-2-freeze10.onnx`，以 RKNN Toolkit2 2.5.0 转换 RV1126B INT8，并推送真板执行。推荐继续做量化调优的产物是**双输出版本**；它通过运行时与后处理验证，但同图检测存在量化退化，尚未通过检测结果一致性门禁，不能据此称为精度验收通过。

| 项目 | 实测结果 |
| --- | --- |
| ONNX | YOLO26l，输入 `[1,3,640,640]`，RGB/255；两类 `helmet=0`、`head=1` |
| ONNX SHA-256 | `25a1a99b991c5057b909899ecd9f0f9a70ed8a7347d96a1a2ca177694ddaff86` |
| 校准集 | 200 张，100 张 PPE train 图、100 帧柜口视频；seed=0，11 段视频均有抽帧 |
| 量化 | `rv1126b`、`w8a8`、`normal`、默认 channel 量化；非量化算子仍可能使用 FP16 |
| 双输出 RKNN | `runs/rknn/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn` |
| RKNN 大小 | 28,835,752 bytes，约 27.5 MiB |
| RKNN SHA-256 | `1ca2ad5809b1add183fc33f7d09e9dc19b6d8492b10d7e63b68320c45a3bd92c` |
| 板型 | `Rockchip RV1126B XIAOYU 50IPC V10 Board`，Linux 6.1.157，AArch64 |
| ADB | `192.168.1.145:5555`；已读取 device tree 核实，不依赖 ADB 的 Nexus_4 标记 |
| 板端模型 | `/tmp/flow_demo/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn` |
| Runtime | API `2.3.2 (429f97ae6b@2025-04-09T09:09:27)`，driver `0.9.8` |
| 输入 | `uint8 NHWC pass_through=0`；模型查询为 `int8 NHWC 1x640x640x3`，`zp=-128`，`scale=0.00392156886` |
| 板端推理 | 3 张原始柜口帧，各 10 次浮点取回 + 1 次原生 INT8 取回，共 33 次，全部 `RKNN_SMOKE_OK` |
| 耗时 | 30 次浮点取回平均 128,053.7 µs，范围 124,192–134,121 µs，约 7.8 次/秒 |
| 板端 C 后处理 | 每张运行 10 次，TopK/分数反量化平均约 2.33–2.60 ms；不含文件读入、JSON 输出和原图坐标映射 |

推理耗时包含 `rknn_inputs_set`、`rknn_run` 与 `rknn_outputs_get`，不含图像解码、letterbox、跟踪和媒体调度；不能视为端到端视频帧率。模型、smoke/C 程序及三份 RGB 输入均经过 ADB 回读并在 PC 核验 SHA-256。

## 为什么拆分输出

首次完整图转换成功，模型为 `full/comp-2-freeze10-rv1126b-int8.rknn`，28,873,487 bytes，SHA-256 为 `412af4e125e3a6f3bb32534dc8f74581be36e5749a3200801dd5df9ce374fd37`。它在板端加载与重复推理通过，10 次平均约 135 ms。

但最终 `[1,300,6]` 张量把大坐标与 `[0,1]` 的置信度/类别合并量化，查询到 `int8 zp=-113 scale=3.09245634`。同图实际输出的全部置信度和类别均变成 0；`want_float=1` 只是反量化已损失的数据，不能恢复置信度。该版本保留为问题证据，不作为部署候选。

第二次转换保留同一 ONNX 的两个中间结果，分别量化：

| 索引/节点 | 形状 | 原生类型/布局 | 零点 | 尺度 |
| --- | --- | --- | ---: | ---: |
| 0 `/model.23/Sigmoid_output_0` | `[1,2,8400]`，两类概率 | `int8 / UNDEFINED` | -128 | 0.00378100411 |
| 1 `/model.23/Mul_2_output_0` | `[1,4,8400]`，已解码 xyxy | `int8 / UNDEFINED` | -74 | 5.7338891 |

`UNDEFINED` 为运行库实际报告的格式；后处理按上述明确形状的通道顺序读取，不能把它强行当成 NHWC。此拆分节点选择与本机 Rockchip model zoo 的 YOLO26 示例一致。原 ONNX 保留不变；拆分图另存于 `split/comp-2-freeze10-split.onnx`。

CPU 后处理先按每个 anchor 的最大类别分数选 300 个 anchor，再在这 300×2 个分数中取前 300 个，复现原图两阶段 TopK。它允许一个 anchor 的两个类别都入选，不将其简化为每 anchor 只取一个类别；不额外加 sigmoid、DFL 或 NMS。INT8 同分时采用 anchor/类别原顺序保证可复现，原浮点图低分同分项的框顺序不作为精度证据。

## 同图结果与限制

三张测试帧来自 `单项取出.mp4`、`单项放入.mp4`、`合并遮挡.mp4`，取各视频约 42% 的位置。保存了原始 PNG、RGB、视频 SHA-256、帧序号、时间和 letterbox sidecar。使用 `conf=0.25`：

| 测试帧 | 原 ONNX | 板端 INT8 |
| --- | --- | --- |
| sample-0，单项取出 | 0 helmet、0 head | 0 helmet、0 head |
| sample-1，单项放入 | 0 helmet、0 head | 0 helmet、0 head |
| sample-2，合并遮挡 | 2 helmet、1 head | 1 helmet、0 head |

sample-2 的最高 helmet 分数从 `0.8632007` 降为 `0.3629764`；匹配框 IoU 为 `0.87647`，模型输入坐标最大差约 `7.03 px`。这说明当前 normal INT8 在此样本有明显退化；不是通过调整结果显示阈值就能宣称与 ONNX 一致。

已完成的分级验证：

- **PC 浮点拆分/末端逻辑**：三图分数排序及 `conf≥0.25` 检测行与原 ONNX 的最大误差均为 0。
- **交叉编译**：smoke 和 YOLO26 C 后处理使用 SDK AArch64 编译器，以 `-Wall -Wextra -Werror` 编译通过。
- **板端运行**：双输出模型 33 次推理通过，C 后处理已在真板执行。
- **张量/后处理对照**：三图原生 INT8 反量化与运行库浮点输出最大差为 0；板端 C 与 Python 的最大坐标/分数差约 `4.2e-7`。
- **检测质量**：三张未标注图仅用于同图 smoke；检测结果一致性未通过，未计算新模型 mAP。校准使用过的柜口素材不能再称为独立精度 holdout。
- **未执行**：新模型的完整标注集评测、板端视频流水线回放、跟踪/穿越、实时摄像头。

Toolkit 对首层卷积及部分分类分支权重报告了离群值。后续量化调优可对照 MMSE/混合量化及更贴近训练分布的校准集，逐层定位分数退化；本轮未执行这些额外转换，不将其描述为已解决。

## 复现与日志

从 `flow_demo/` 执行转换，使用新的 run 名称避免覆盖：

```sh
RUN=runs/rknn/comp-2-freeze10-int8-new
.venv-rknn310/bin/python training/prepare_rknn_calibration.py \
  --dataset dataset --videos dataset/raw_videos --output-dir "$RUN/calibration"
OMP_NUM_THREADS=8 OPENBLAS_NUM_THREADS=8 \
  .venv-rknn310/bin/python training/convert_rknn.py \
  --onnx comp-2-freeze10.onnx --dataset "$RUN/calibration.txt" \
  --output-dir "$RUN/split" --yolo26-split
make -C board
```

本轮实际环境为 Python 3.10.20、Torch 2.4.0+cu121（转换计算未使用 CUDA）、NumPy 1.26.4、ONNX 1.17.0、ONNX Runtime 1.23.2、Toolkit2 2.5.0。完整环境在 run 的 `environment.txt`。重建环境可先安装 CPU Torch 2.4.0，再安装 `training/requirements-rknn.txt` 和适配 Python ABI 的本地 Toolkit 2.5.0 wheel。

现有板端文件可直接复测，不覆盖 dump：

```sh
adb -s 192.168.1.145:5555 shell \
  'LD_LIBRARY_PATH=/oem/usr/lib /tmp/flow_demo/comp-2-freeze10-int8-20261111/rknn_smoke \
   --model /tmp/flow_demo/comp-2-freeze10-int8-20261111/split/comp-2-freeze10-rv1126b-int8.rknn \
   --input /tmp/flow_demo/comp-2-freeze10-int8-20261111/sample-2.rgb --runs 10 --want-float 0'
adb -s 192.168.1.145:5555 shell \
  '/tmp/flow_demo/comp-2-freeze10-int8-20261111/split/rknn_yolo26_postprocess \
   --dump-dir /tmp/flow_demo/comp-2-freeze10-int8-20261111/split/sample-2-native \
   --score-zp -128 --score-scale 0.00378100411 \
   --box-zp -74 --box-scale 5.7338891 --conf 0.25 --runs 10'
```

上述量化参数只适用于本轮指定哈希的模型；任何重新转换后都要重新查询。该固件 ADB 只支持 PTY shell，`exec-out`/`shell -T` 不可用；二进制文件应通过 `adb push/pull` 传输和核验，不经 shell `cat` 重定向。

PC 上重新解析已保存的板端 dump：

```sh
.venv-rknn310/bin/python training/compare_yolo26_board.py \
  --run-dir runs/rknn/comp-2-freeze10-int8-20261111 \
  --board-dir board-split --split --require-c
```

主要证据都在 `runs/rknn/comp-2-freeze10-int8-20261111/`：

- `calibration.json`、`calibration.txt`：校准来源、抽帧及预处理哈希。
- `full/`、`full.console.log`：完整图转换及输出归零证据。
- `split/`、`split.console.log`：双输出图、RKNN、转换元数据与 Toolkit 日志。
- `samples/`、`onnx-baseline.json`、`split-onnx-parity.json`：固定输入、原图输出及拆分浮点对照。
- `board-split/deployment.json`、`commands.json`：板型、传输文件/运行库哈希和实际命令。
- `board-split/sample-*-float.log`、`sample-*-native.log`、对应 dump：板端属性、量化参数、重复推理耗时和输出。
- `board-split/sample-*-c.json`、`sample-*-c.log`、`comparison.json`：板端 C 输出、后处理耗时及 Python/ONNX 对照。
- `board-split/sample-*-detections.jpg`：板端检测框映射回原视频帧的预览。

相关自动测试覆盖运行时量化参数、错误张量/类别、两类 TopK、量化同分时的 C/Python 一致性、输入 sidecar 和旧模型后处理回归，共 15 项通过。
