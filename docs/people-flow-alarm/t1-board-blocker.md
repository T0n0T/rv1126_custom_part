# T1 真板门禁阻塞记录

日期：2026-08-27（Asia/Shanghai）

## 当前结论

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
  cat /etc/os-release
  ls -l /oem/usr/lib/librockiva.so /oem/usr/lib/librknnrt.so \
        /oem/usr/lib/iva_object_detection_v3_pfp.data
  MODEL_PATH=/oem/usr/lib ROCKIVA_LIB_DIR=/oem/usr/lib INPUT=/tmp/input.nv12 \
  WIDTH=640 HEIGHT=360 FRAMES=30 FPS=10 MODEL=pfp \
  MIN_PERSON=1 MIN_TRACKING=1 /tmp/run_probe.sh
' 2>&1 | tee t1-rockiva-board.log
```

输入文件必须来自包含行人的代表性场景，并且严格为
`width * height * 3 / 2` 字节/帧；30 帧的 `640x360` 文件总计
`10,368,000` 字节。全零或其他合成空场景只能验证格式和帧所有权，不能作为
person/tracking 证据。CPU 地址探针成功后，仍必须用真实采集 DMA-BUF 重复
生命周期测试，并记录 `objId/state`、推理时延、NPU/CPU/内存、温度、丢帧和
主视频不受影响的证据，才可解除 T3。
