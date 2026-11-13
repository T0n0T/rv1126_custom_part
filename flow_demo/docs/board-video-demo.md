# RV1126B 视频帧到 RKNN 板端验证

## 192.168.1.63 全片分批实测

2026-11-11 在用户指定的 `192.168.1.63:5555` 上完成 `单项取出.mp4` 全片检测。使用 `comp-2-freeze10.onnx` 对应的双输出 INT8 RKNN，置信度为 0.25；PC 解码和预处理后，按 30 帧分批推送，在板端执行 RKNN 和 C 后处理，再拉回并顺序合并 JSONL。

| 项目 | 实测结果 |
| --- | --- |
| ADB server | 5038 |
| 板型 / Linux | Rockchip RV1126B XIAOYU 50IPC V10 Board / 6.1.157 |
| Runtime / driver | 2.3.2 / 0.9.8 |
| 全片帧数 | 168，源帧号 0–167，partial=false |
| 源 PTS | 0–5573.322 ms，全部与输入 TSV 一致 |
| 分批 | 6 批：30、30、30、30、30、18 帧，全部报告 RKNN_YOLO26_INFER_OK |
| RGB 推送 | 共 206,438,400 bytes，六批推送累计 63.713 s |
| 平均推理 / 后处理 | 128.494 / 3.208 ms/帧，按各批帧数加权 |
| 检测候选 | helmet 92、head 27，96 帧含候选 |
| 模型与程序传输 | 板端文件回读后的 SHA-256 均与 PC 一致 |

候选数是各帧检测框的累计值，不是独立物体数、动作数或准确率。推理和后处理耗时不含 PC 解码、预处理、模型加载、ADB 传输与跟踪，不能作为端到端实时帧率。当前 INT8 的同图量化退化仍待解决。

记录目录：`runs/board-video/full-single-takeout-63-chunked-20261111-165829/`。`validation.json` 核验了完整帧数、连续帧号、逐帧源 PTS、合法坐标/分数、分批日志及合并结果；`transport-verification.json` 保存模型和程序的 PC/板端回读哈希。PC 上分批 RGB 拼接后的 SHA-256 与完整帧流一致；板端每批输入在推理后清理，未逐批回读，不能将本轮描述为完整输入的板端回读哈希核验。

此前 ADB shell 超时、单一连接重试后仍 offline；用户恢复设备后，本次六批全部成功。入口已加入视频预处理前的 RV1126B 板型预检（默认 15 秒）及每次 ADB 调用超时（默认 120 秒），可用 ADB_PREFLIGHT_TIMEOUT 和 ADB_TIMEOUT 调整。离线或无响应会保留 adb-preflight.log 并退出，不自动重启设备。对应四项回归测试及训练工具其余测试共 26 项通过。

这次 2026-11-11 的单视频检测 run 没有传入区域配置，因此该 run 未执行真实视频的 BoT-SORT、穿越与动作事件。随后已为 11 段视频分别生成并人工确认区域 profile，并完成了下文记录的整片事件回放。板端本机 GStreamer 解码和实时摄像头仍待验证。

## 11 段视频整片行为链路回放

2026-11-12 使用每段视频对应的已确认 profile，在 `192.168.1.63:5555` 完成 11 段视频的板端 RKNN 检测和 BoT-SORT／区域事件回放。检测阈值为 `CONF=0.25`；PC 解码并按 30 帧分批推送 RGB，板端执行检测、跟踪和事件判定。

| 视频 | 帧数 | helmet/head 候选框 | 确认取出/放入 | 不确定事件 |
| --- | ---: | ---: | ---: | ---: |
| 单项取出 | 168 | 92/27 | 0/0 | 1 |
| 单项放入 | 166 | 32/5 | 0/0 | 0 |
| 合并遮挡 | 254 | 108/107 | 0/0 | 1 |
| 同时通过 | 176 | 100/9 | 0/0 | 2 |
| 多次取放 | 345 | 144/66 | 0/0 | 0 |
| 开关门 | 171 | 5/29 | 0/0 | 0 |
| 手臂遮挡 | 161 | 22/6 | 0/0 | 0 |
| 抖动 | 96 | 23/22 | 0/0 | 1 |
| 折返 | 163 | 112/0 | 0/0 | 1 |
| 柜内位移 | 231 | 68/0 | 0/0 | 0 |
| 空手不取 | 221 | 10/44 | 0/0 | 0 |

11 段合计 2152 帧，所有输入和检测结果均为完整视频，无 partial；候选框累计 716 个 helmet、315 个 head，确认事件为 0，不确定事件共 6 个。加权平均推理和后处理耗时分别为 128.312 ms/帧和 3.267 ms/帧，RGB 推送总量为 2,644,377,600 bytes。结果目录为 `runs/board-video/board-20261112-*`。

这里的 `summary.result_complete=false` 表示存在不确定事件，不表示视频帧没有处理完；每段的 `processed_through_frame` 均到达末帧。该轮证明了全片输入、板端检测、跟踪和事件程序可以连续运行，行为事件验收未通过，不能据此判断检测准确率或动作召回率。

## 三段正样本低阈值复测

2026-11-13 对明确包含取放动作的三段视频使用同一板端模型复测：`CONF=0.10`，BoT-SORT `track_low_thresh=0.05`、`track_high_thresh=0.20`、`new_track_thresh=0.20`，`min_track_score=0.10`，最大观测间隔仍为 400 ms。低阈值 profile 保存在 `/tmp/flow-demo-threshold-trials/`，没有覆盖正式标定文件。

| 视频 | 帧数 | helmet/head 框：0.25 → 0.10 | 确认取出/放入 | 不确定事件 |
| --- | ---: | ---: | ---: | ---: |
| 单项取出 | 168 | 92/27 → 114/68 | 0/0 | 4 |
| 单项放入 | 166 | 32/5 → 65/36 | 0/0 | 10 |
| 多次取放 | 345 | 144/66 → 176/105 | 0/0 | 16 |

三段共 679 帧，均处理到末帧且 `partial=false`；低阈值下候选框增加，确认事件仍为 0，不确定事件增至 30 个，主要原因为 `observation_gap_exceeded`，另有 `track_started_in_transition`。轨迹复核没有发现同一 helmet track 连续经过 inside、transition、outside 的证据。单项取出在手部遮挡时断检；单项放入手持阶段的 track 12 到第 68 帧后中断，约 2.5 秒后第 143 帧出现不同的 track 25。画面还可见低分 helmet 框落在人颈附近，以及覆盖大面积前景的框，说明降低置信度会带入更多噪声，未补上关键遮挡间隙。

标注预览：`/tmp/flow-demo-threshold-trials/单项取出-review.mp4`、`/tmp/flow-demo-threshold-trials/单项放入-review.mp4`。复测时发现 `单项放入.json` 的 `source_video_sha256` 与视频一致，但 `camera_id/session_id` 曾误写作 `单项取出`；现已更正为 `单项放入`，区域坐标和视频哈希未改变。此前的板端 run 配置仍保留原始运行证据。

此前将 BoT-SORT `track_high_thresh` 和 `new_track_thresh` 降到 0.30（`track_low_thresh` 保持 0.10）的 11 段诊断，共产生 62 个不确定事件，仍未恢复正样本事件；负样本 `柜内位移` 第 181–185 帧还产生了 1 个确定的 `put_in` 误报。因此不能把低阈值或放宽观测间隔直接推广到全部 profile。下一步应先围绕遮挡帧复核检测框与区域归属，再评估更合适的检测目标锚点和轨迹连续性；保留不确定结果，直到负样本回归通过。

## 历史三帧 smoke

2026-11-11 使用 `board/run_rknn_video_demo.sh`，从 `dataset/raw_videos/单项取出.mp4` 解码并预处理前 3 帧，再将帧流推到 RV1126B。板端运行 `comp-2-freeze10.onnx` 对应的双输出 INT8 RKNN，查询真实量化属性，执行两阶段 TopK(300)、坐标还原并写出检测 JSONL。

这次只验证了三帧的输入、NPU 推理和 C 后处理链路。`CONF=0.001` 用于保留候选以验证 JSON 输出；3 帧产生 12 个候选（helmet 3、head 9），不代表检测准确率。当时机位区域尚未经人工确认，因此这次历史 smoke 没有运行动作事件链路。视频解码仍在 PC 完成，未验证板端 GStreamer 视频回放或实时摄像头。

## 设备与模型

| 项目 | 值 |
| --- | --- |
| 板型 | Rockchip RV1126B XIAOYU 50IPC V10 Board |
| SDK API / driver | 2.3.2 / 0.9.8 |
| 模型 | `comp-2-freeze10-rv1126b-int8.rknn` |
| RKNN SHA-256 | `1ca2ad5809b1add183fc33f7d09e9dc19b6d8492b10d7e63b68320c45a3bd92c` |
| 推理程序 SHA-256 | `5f4801c617a856020c76e0b4be12d79df508e687e1913aff6aa27fc883a3d653` |
| 输出 0 | `/model.23/Sigmoid_output_0`，INT8，`zp=-128`，`scale=0.00378100411` |
| 输出 1 | `/model.23/Mul_2_output_0`，INT8，`zp=-74`，`scale=5.7338891` |
| 平均推理 | 128.198 ms/帧 |
| 平均后处理 | 2.617 ms/帧 |

输入是 640×640、NHWC、RGB、uint8，padding 值 114。原始视频帧为 1920×1080，letterbox scale=`1/3`、顶部 padding=`140`。三帧源 PTS 为 0、33.333、66.667 ms。

## 复现

在 `flow_demo/` 执行：

```sh
make -C board
tracking/build_rv1126b.sh
ADB_SERVER_PORT=5038 ADB_SERIAL=192.168.1.63:5555 MAX_FRAMES=3 CONF=0.001 \
  board/run_rknn_video_demo.sh 'dataset/raw_videos/单项取出.mp4' demo-smoke
```

结果位于 `runs/board-video/<RUN_NAME>/`，包括 RGB 帧流、逐帧 PTS/letterbox 元数据、输入与模型哈希、板端 runtime 日志、检测 JSONL 和设备信息。最终构建的板端复现实测目录为 `runs/board-video/demo-release-smoke-20261111/`；模型、程序和完整输入回读后的 SHA-256 与本机一致，核验结果在 `transport-verification.json`。

在转换阶段保存的同一基准图上，新推理入口与板端 C 后处理的 1 个检测框完全一致，归一化坐标及分数的最大绝对误差为 `6.25e-10`；记录在 `runs/board-video/demo-final-smoke-20261111/sample-2-c-parity/`。30 帧连续 smoke 的逐帧日志见 `runs/board-video/demo-30frame-smoke-20261111/`。

完整视频仍需在 PC 保存所有未压缩 RGB，板端默认每次只接收 BATCH_FRAMES=30 帧，推理并拉回结果后清理该批输入。默认最多处理 30 帧；完整处理需显式设置 `MAX_FRAMES=0`。本入口用于模型与跟踪联调，长视频的 PC 存储和 ADB 传输成本仍较高。

此前 168 帧 `单项取出.mp4` 的整文件尝试已完成 PC 解码和预处理，生成 206,438,400 bytes 的 RGB 流；推送到板端超过 5 分钟无进展后中止，未进入 RKNN 推理。对应帧流和 hash 保存在 `runs/board-video/full-single-takeout-20261111/`。该次尝试失败；后续分批传输和全片检测已在本页上方单独记录通过。

此外，同日以默认 `CONF=0.25` 处理了该视频前 30 帧，源 PTS 范围 0–973.322 ms。36,864,000 bytes RGB 输入推送耗时 11.765 s；板端报告平均推理 127.568 ms/帧、后处理 3.580 ms/帧，输出 16 个候选（15 helmet、1 head）。记录保存在 `runs/board-video/demo-30frame-smoke-20261111/`。这仍是约 1 秒视频片段，且未经人工标注，候选数量不能作为准确率或动作结论。

## 后续门禁

- 对独立标注的完整样本集评估该 INT8 模型；当前 smoke 不报告 mAP 或动作准确率。
- 11 段视频的 zones profile 已分别人工确认并完成板端事件回放；接下来补逐次动作、数量、方向及开关门 PTS 真值，复核遮挡轨迹，再用同一批负样本检查修订后的事件规则。
- 评估传输和完整回放性能后，再将 PC 解码迁移到 RV1126B GStreamer，并记录丢帧、源 PTS、NPU/后处理耗时及事件输出。
