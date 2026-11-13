# RV1126B RKNN smoke 结果

新增指定模型 `comp-2-freeze10.onnx` 的 YOLO26l INT8 转换与 33 次真板推理见 [专项记录](comp-2-freeze10-rknn.md)。其双输出模型与 C 后处理运行通过，但存在同图量化掉检；下文 YOLOv8n 的六头参数不能用于该模型。

## 2026-11-11：FP16 与 INT8

本次使用真实板 `192.168.1.63:5555`，型号 `Rockchip RV1126B XIAOYU 50IPC V10 Board`，内核 `Linux 6.1.157`，AArch64。板端运行库为 `/oem/usr/lib/librknnrt.so`，文件大小 7,726,232 bytes。RKNN 运行时报告：

```text
api=2.3.2 (429f97ae6b@2025-04-09T09:09:27)
driver=0.9.8
```

输入由 `training/prepare_rknn_input.py` 生成，契约为 640×640、RGB、NHWC、uint8 原始字节，文件大小 1,228,800 bytes。由于样本本身就是 640×640，本次 sidecar 的 `scale=1`、`pad_left=0`、`pad_top=0`。

| 模型 | SHA-256 | 输入属性 | 输出属性 | 5 次平均耗时 | 结果 |
| --- | --- | --- | --- | ---: | --- |
| `helmet-yolov8n-fp16-tk250/helmet-yolov8n-rv1126b-fp16.rknn` | `cc0dff67207ea3264c18e3b2760e62c5a2059084a5b630d424d807653d73ae57` | `float16 NHWC 1x640x640x3`, `zp=0`, `scale=1`；运行时以 `uint8 NHWC pass_through=0` 提交 | 1 个 `float16` 输出，`1x5x8400` | 118,109 µs | `RKNN_SMOKE_OK` |
| `helmet-yolov8n-heads-int8-eval/helmet-yolov8n-rv1126b-int8.rknn` | `c06667175be8bc79f1cd6ed903b5cc38fcc7c05053d1f094f83218d999af1b04` | `int8 NHWC 1x640x640x3`, `zp=-128`, `scale=0.00392156886`；运行时以 `uint8 NHWC pass_through=0` 提交 | 6 个 `int8 NCHW` head；`s8/s16/s32` 的 box 与 score 量化参数已由工具打印 | 21,159 µs | `RKNN_SMOKE_OK` |

INT8 六个输出的板端属性如下，后处理必须使用这些参数反量化：

```text
box_logits_s8    zp=-14  scale=0.395262748  shape=1x64x80x80
helmet_scores_s8 zp=-128 scale=0.00334408041 shape=1x1x80x80
box_logits_s16   zp=-24  scale=1.18675947  shape=1x64x40x40
helmet_scores_s16 zp=-128 scale=0.00356173562 shape=1x1x40x40
box_logits_s32   zp=-2   scale=0.711200655 shape=1x64x20x20
helmet_scores_s32 zp=-128 scale=0.00371879875 shape=1x1x20x20
```

## 结论与下一门

- RKNN 模型加载、输入尺寸/布局协商、重复推理和输出释放已在真板通过。
- INT8 额外以 `want_float=0` 取回原生输出，六个 buffer 字节数分别为 `409600/6400/102400/1600/25600/400`，与查询到的 `int8` 元素数一致。
- FP16 模型可以作为后处理和 PC/板端同图对照的第一基线；单次推理约 120 ms，暂不满足实时性能结论。
- INT8 模型的运行时属性与转换产物的六头结构一致；单次推理约 21 ms，但这只代表 `rknn_run` 加输出取回，不包含图像解码、预处理、后处理、跟踪和 GStreamer 调度。
- 当前已完成 PC 端 YOLO 后处理参考实现和 AArch64 C 版本交叉编译；C 版本尚未在目标 RV1126B 上执行。真实柜口视频板端回放、BoT-SORT 跟踪和穿越事件仍未验证，不能把本结果写成端到端验收通过。

## PC 端 INT8 六头参考后处理

`training/postprocess_rknn_int8.py` 已实现原生 int8 张量反量化、DFL 16-bin 解码、置信度筛选、单类别 NMS 和 letterbox 坐标映射。可移植 C 版本 `board/rknn_int8_postprocess.c` 已通过 SDK AArch64 交叉编译。单元测试使用合成张量覆盖量化公式、DFL 几何、NMS、阈值和坐标还原；板端 dump 也已解码，原始结果写在 `results/rknn-dump-20261111/detections-int8.json`。

PC 上分别运行 C 与 Python 解析同一 dump 后，300 个输出框顺序一致，最大坐标差约 `0.000062 px`，最大分数差约 `1.2e-10`。这只验证两份后处理实现对已保存张量的一致性，不等同于已在 RV1126B 执行 C 程序。
逐框比较的模型及六个 dump 文件 SHA-256 保存在 `results/rknn-dump-20261111/postprocess-parity.json`。本轮 ADB 可见设备不是 RV1126B，因此没有将 C 程序推送到该设备运行。

当前 dump 使用评测参数 `conf=0.001 / IoU=0.7 / max_det=300`，结果达到 300 框上限，其中 298 个框置信度约 `0.00356`，最高分为 `0.58769`。这是低置信度评测候选集，查看可读的检测结果应使用更高阈值，例如 `--conf 0.25`。原始 dump 没有记录输入 RGB 的哈希或 sidecar，因此还不能用它确认预测框与某张源图逐框一致。

已有 `test-metrics.json` 和 `test-statistics.npz` 保存通用 PPE 测试集的聚合指标及 TP/置信度统计，但没有逐图预测框或逐图 RKNN 原始输出。故当前只能报告转换评测已经保存的 ONNX/RKNN mAP 差异，不能声称新解析器重新复现了这些指标。

下一步按顺序实现：

1. 在目标 RV1126B 上执行已交叉编译的 C 后处理，并在一次板端推理中同时保存输入 RGB、sidecar、输入 SHA-256 和六个输出，固定可复现的同图基准。
2. 在同一批原始输出上对照板端 C 与 Python 结果；再跑完整测试集，生成可追溯的逐图评测结果。
3. 对柜口视频做板端文件回放，记录 PTS、检测数量、推理/后处理耗时和丢帧，再接入跟踪与三区域穿越。
