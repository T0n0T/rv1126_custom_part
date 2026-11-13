# 机位区域 profile

每个需要单独处理的拍摄样本维护一份 profile JSON。由于手机拍摄时机位可能变化，当前约定使用对应视频文件名（去掉扩展名）命名 profile，例如 `单项取出.mp4` 对应 `tracking/config/local/单项取出.json`。profile 保存标定时的画面几何，并记录源视频 SHA-256；换用另一段视频前，先确认视角和柜口位置一致，否则为该视频重新标定。

用标定器生成本地 profile：

```sh
.venv-rknn310/bin/python tracking/calibrate_zones.py \
  --video dataset/raw_videos/单项取出.mp4 \
  --camera-id 单项取出 \
  --output tracking/config/local/单项取出.json \
  --display-scale 0.7
```

配置至少包含：

- `camera_id`：机位或拍摄样本标识；当前手机样本可使用视频主文件名，例如 `单项取出`，固定机位可使用 `left-corner` 或 `right-corner`；
- `geometry_confirmed`：人工检查完采样帧后才设为 `true`；
- `zones.inside`、`zones.transition`、`zones.outside`：原始画面归一化多边形；
- `calibration.source_width/source_height`：标定时的画面分辨率；
- `calibration.reviewed_frames`：人工检查过的帧号和源 PTS；
- `source_video_sha256`：标定素材的身份记录。

如果两段视频确认来自完全相同的固定视角，可以复用其中一份 profile；为便于追溯，仍建议为每段视频保留同名 profile，或在运行记录中明确写出复用关系。文件名相同但内容不同的素材应增加唯一前缀或目录，不能覆盖已有 profile。

事件运行入口会将 profile 复制成当前 run 的 `zones.json`，替换当次 `video`、`session_id`，并记录实际视频 SHA-256 和分辨率。输入视频与标定画面长宽比不一致时会拒绝运行。相机移动、镜头变化、裁剪或柜体位置变化后，必须重新生成 profile。`tracking/config/local/` 被 `.gitignore` 排除，实际部署配置应由设备配置管理保存；`tracking/examples/` 中的示例只能用于接口测试。
