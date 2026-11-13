# 跟踪与穿越核心

`flow_demo_cli` 接收一行一帧的检测 JSONL，使用 BoT-SORT 关联轨迹，再按归一化 `inside`、`transition`、`outside` 区域生成确认事件和不确定事件。输入框坐标是相对原始视频画面的 `[0,1]` 坐标；源视频 PTS 必须单调。事件及摘要格式见 `../docs/spec.md`。

PC 构建与测试：

```sh
cmake -S tracking -B tracking/build
cmake --build tracking/build -j4
ctest --test-dir tracking/build --output-on-failure
```

使用工作树上层 RV1126B SDK 的 AArch64 工具链交叉编译：

```sh
tracking/build_rv1126b.sh
```

默认产物为 `tracking/build-rv1126b/flow_demo_cli`。如果 SDK 位于其他目录，可设置 `SDK_ROOT=/path/to/rv1126b_sdk`；也可以用 `CXX=/path/to/aarch64-g++` 或 `BUILD_DIR=/path/to/build` 覆盖。该 CLI 处理已完成检测的 JSONL；板端视频联调通过 `board/run_rknn_video_demo.sh` 接入它，实时摄像头通过 `board/flow_events.cpp` 复用同一事件核心。用法见 [板端工具](../board/README.md) 与 [摄像头说明](../docs/board-camera-demo.md)。

板端核心冒烟测试只使用合成检测框，不验证模型、相机区域、真实视频跟踪或动作准确率。对真实素材运行 `tracking/run_demo.sh` 前，应先人工审查相机画面并设置 `geometry_confirmed=true`。

## 摄像头区域标定

每个需要单独处理的视频使用一个区域 profile，坐标保存在 JSON，不写入检测器或跟踪器。手机拍摄角度可能变化，因此 profile 默认使用视频文件名（去掉扩展名）命名，例如 `单项取出.mp4` 对应 `tracking/config/local/单项取出.json`。标定器用九张等间距采样帧复核视野，依次点击 inside、transition、outside 的多边形顶点，按安全帽检测框中心实际经过的区域划分。

```sh
.venv-rknn310/bin/python tracking/calibrate_zones.py --video dataset/raw_videos/单项取出.mp4 --camera-id 单项取出 --output tracking/config/local/单项取出.json --display-scale 0.7
```

鼠标左键添加顶点；1/2/3 切换区域，n 前进，u 撤销，r 重画，[ 和 ] 查看采样帧，y 人工确认并保存，q 取消。按 y 前必须逐张看过所有采样帧，且区域不能自交或相互重叠。配置会记录画面尺寸、采样帧号/PTS、视频 SHA-256 和 geometry_confirmed=true。视角、俯仰、距离、变焦、裁剪或柜体位置发生变化时，不能只因为分辨率相同就复用 profile，应为新视频重新标定。

事件运行时，脚本把 profile 复制进新的 run 配置，并替换当次 session_id、video 及输入 SHA-256；后续视频可复用同一相机 profile。移动相机、更换镜头或改变画面裁剪后重新标定。若柜门遮挡导致穿越不可见，算法仍应保留不确定结果。

手机 MP4 可能在容器中写入 `Display Matrix` 旋转信息，而编码像素本身仍保持未旋转方向。项目的标定、视频预处理、PC 推理和预览工具统一通过 `video_io.open_video` 在第一次读取前启用 OpenCV 的 `CAP_PROP_ORIENTATION_AUTO`，区域坐标以播放器看到的显示方向为准，不要再手工旋转同一段视频。当前素材中 `合并遮挡.mp4` 带有 `rotation=-180`，这正是标定窗口看起来倒置的原因；修复后会自动按正常方向读取。
