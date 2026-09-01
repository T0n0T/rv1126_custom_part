# T1 真板连接与资源阻塞记录

记录日期：2026-08-27 起，最近更新：2026-09-01（Asia/Shanghai）

> 2026-08-28 已通过 ADB 恢复真板连接并完成 CPU-NV12 RockIVA 探针；2026-08-31
> 又完成隔离 `/dev/video25` 的原生 V4L2 DMA-BUF 生命周期探针，2026-09-01
> 取得一轮 60 帧有人场景结果。连接阻塞已解除，但 T1 仍未完成：代表性重复场景、
> 完整流边界和主视频隔离仍是硬门禁。实测结果见
> `t1-board-result.md`。

## 2026-08-31 起设备资源上限暂停

当前设备资源已达到本轮允许上限，暂停更高占用和并发板端复测；不再增加
RockIVA、V4L2 或采集管线占用，也不操作 `/dev/video24`。本轮仅保留一轮短时
60 帧有人场景结果作为候选证据。这是设备资源/测试窗口上限导致的暂停，不是
RockIVA 检测失败：

- CPU-NV12 有人片段已经取得 person 检出、`FIRST/TRACKING/LOST/DISPEAR` 状态和
  回调释放闭环的正向证据；
- `/home/Tiger/Documents/rtsp_demo/test1.mp4` 生成的密集人群 CPU-NV12 样本也已完成
  60 帧检测/释放闭环，可用于压力观察但不替代 DMA-BUF 验收；
- 隔离 `/dev/video25` 已取得原生 V4L2 单物理平面 DMA-BUF 的 capture/push/detect/
  release 生命周期及短时停止/重启证据；
- 已取得一轮有人场景下的 DMA-BUF 检出/tracking 候选结果，但仍缺少代表性重复场景、
  完整流 epoch（含异步停止回调和长期重复启停）以及主编码连续性/点播并发证据。

因此 T1 保持 `in progress`，T3 继续保持阻塞。资源恢复前不得用单次候选结果、空场景、
CPU-NV12 结果或已有生命周期结果替代完整验收边界。

## 资源恢复后的最小 DMA-BUF 复测入口

仅在设备资源恢复并确认可以进行下一轮短时隔离试验后，使用 `/dev/video25`，不设置
`ALLOW_MAINPATH=1`，优先重复有人场景并保存 probe 直接退出码：

```sh
SDK=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
PROBE=$SDK/custom_part/media_engine/tests/board/rockiva_probe
ADB=192.168.1.63:5555

make -C "$PROBE" v4l2_rockiva_probe SDK_ROOT="$SDK"
adb -s "$ADB" get-state
adb -s "$ADB" push "$PROBE/v4l2_rockiva_probe" /tmp/v4l2_rockiva_probe
adb -s "$ADB" push "$PROBE/run_v4l2_rockiva_probe.sh" /tmp/run_v4l2_rockiva_probe.sh
adb -s "$ADB" shell 'chmod 755 /tmp/v4l2_rockiva_probe /tmp/run_v4l2_rockiva_probe.sh
  DEVICE=/dev/video25 MODEL_PATH=/oem/usr/lib ROCKIVA_LIB_DIR=/oem/usr/lib \
  WIDTH=640 HEIGHT=360 FRAMES=60 MODEL=pfp MIN_PERSON=1 MIN_TRACKING=1 \
  ALLOW_MAINPATH=0 /tmp/run_v4l2_rockiva_probe.sh >/tmp/t1-v4l2-rockiva-person.log 2>&1
  probe_status=$?
  cat /tmp/t1-v4l2-rockiva-person.log
  printf "PROBE_DIRECT_EXIT_STATUS=%s\n" "$probe_status"
  exit 0'
```

复测至少记录 `objId/state` 生命周期、`sequence_errors`、callback 计数、推理/释放
时延和直接退出码；只有在多轮有人场景结果可重复后，才继续安排完整 epoch、主编码/
点播并发和长期资源边界验证。`adb shell` 的传输退出码不能代替日志中的
`PROBE_DIRECT_EXIT_STATUS`。

## 2026-08-27 历史结论

T1 探针已经可以由 SDK 工具链交叉编译，但本次没有可用的 RV1126B 真板连接。
目标板地址为 `192.168.1.63`；主机的有线和无线接口分别为
`192.168.1.88`、`192.168.1.47`。分别绑定两个接口复测后，到目标的邻居解析
仍停留在 `INCOMPLETE/FAILED`，`ping` 均无响应，TCP/22 连接超时；
`adb devices -l` 为空，`adb connect 192.168.1.63:5555` 返回
`No route to host`。没有 USB 或串口设备可作为替代连接。没有板端 RockIVA
输出，因此不能填写检测、跟踪、释放或资源指标。

静态准备已确认：

- `aarch64-rockchip1240-linux-gnu-gcc` 可用，探针产物为 AArch64 ELF；
- staging 中存在 `librockiva.so`、`librknnrt.so` 和
  `iva_object_detection_v3_pfp.data`，`oem` 输出也包含对应文件；
- 当前未确认板端实际模型可加载、NPU 核心掩码、输入方式或许可证状态。

探针默认要求至少观察到 1 个 person 结果和 1 个
`ROCKIVA_OBJECT_STATE_TRACKING` 结果；否则即使 init、push、wait 和 release
均成功，进程仍以非零状态退出。`MIN_PERSON=0 MIN_TRACKING=0` 只允许用于空场景
或负向诊断，不能作为 T1 检测/跟踪证据。该门槛也不证明 `FIRST` 与 `TRACKING`
属于同一 `objId`，不衡量 ID switch 或遮挡恢复质量；这些结果必须从代表性真板
录制场景中统计后才能完成 T1。

## 2026-08-31 开机后重试

用户确认 IPC 已开机后，在主机 `enp0s31f6`（`192.168.1.88/24`）上重新执行
ADB、ARP 邻居、ping 和 TCP/5555 只读检查。路由仍指向该接口，但
`192.168.1.63` 邻居状态为 `INCOMPLETE/FAILED`，ADB 返回 `No route to host`，
ping 仍为全丢包；本次没有推送或运行任何探针，也没有打开 `/dev/video24` 或
`/dev/video25`。局域网发现只看到 `192.168.1.1`、`192.168.1.88` 和
`192.168.1.99` 在线，未发现目标板的新地址。

该次早先重试的阻塞是板卡网络/地址可达性，不是 RockIVA 探针或生产代码错误。
随后 ADB 已恢复到 `192.168.1.63:5555`，并完成 CPU-NV12、隔离 DMA-BUF、
两次原生 V4L2->RockIVA 重启边界和一轮 60 帧有人场景复测；最新结果见
`t1-board-result.md`。当前实际阻塞已收敛为代表性重复场景、完整流边界、主视频连续性
和生产输入协商证据不足，因此不解除 T1，也不推进 T3 生产分析分支。

## 板卡恢复后的最小流程

```sh
SDK=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
PROBE=$SDK/custom_part/media_engine/tests/board/rockiva_probe
BOARD=192.168.1.63
ADB=$BOARD:5555
INPUT_HOST=/path/to/person-640x360-30.nv12

make -C "$PROBE" SDK_ROOT="$SDK"
INPUT_BYTES=$(stat -c %s -- "$INPUT_HOST") || exit 1
[ "$INPUT_BYTES" -eq 10368000 ] || {
  printf 'unexpected input size: %s bytes\n' "$INPUT_BYTES" >&2
  exit 1
}
adb connect "$ADB"
adb -s "$ADB" get-state
adb -s "$ADB" push "$PROBE/rockiva_probe" /tmp/rockiva_probe
adb -s "$ADB" push "$PROBE/run_probe.sh" /tmp/run_probe.sh
adb -s "$ADB" push "$INPUT_HOST" /tmp/input.nv12
adb -s "$ADB" shell 'chmod 755 /tmp/rockiva_probe /tmp/run_probe.sh'
adb -s "$ADB" shell '
  uname -a
  tr -d "\000" </proc/device-tree/model; echo
  [ -r /etc/os-release ] && cat /etc/os-release || true
  ls -l /oem/usr/lib/librockiva.so /oem/usr/lib/librknnrt.so \
        /oem/usr/lib/iva_object_detection_v3_pfp.data
  MODEL_PATH=/oem/usr/lib ROCKIVA_LIB_DIR=/oem/usr/lib INPUT=/tmp/input.nv12 \
  WIDTH=640 HEIGHT=360 FRAMES=30 FPS=10 MODEL=pfp \
  MIN_PERSON=1 MIN_TRACKING=1 /tmp/run_probe.sh >/tmp/t1-rockiva-probe.log 2>&1
  probe_status=$?
  cat /tmp/t1-rockiva-probe.log
  printf "PROBE_DIRECT_EXIT_STATUS=%s\n" "$probe_status"
  exit 0
' >t1-rockiva-board.log 2>&1
rg -a '^PROBE_DIRECT_EXIT_STATUS=0$' t1-rockiva-board.log
```

`adb shell` 的传输退出码不能代替 probe 进程退出码；必须检查日志中的
`PROBE_DIRECT_EXIT_STATUS`。

输入文件必须来自包含行人的代表性场景，并且严格为
`width * height * 3 / 2` 字节/帧；30 帧的 `640x360` 文件总计
`10,368,000` 字节。全零或其他合成空场景只能验证格式和帧所有权，不能作为
person/tracking 证据。隔离节点的真实采集 DMA-BUF 生命周期已单独通过，但仍必须
在有人场景下补多轮 `objId/state` 稳定性、推理时延、NPU/CPU/内存、温度、丢帧和
主视频不受影响的证据，另补采集重启边界，才可解除 T3。
