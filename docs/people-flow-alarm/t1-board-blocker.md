# T1 真板门禁阻塞记录

日期：2026-08-27（Asia/Shanghai）

## 当前结论

T1 探针已经可以由 SDK 工具链交叉编译，但本次没有可用的 RV1126B 真板连接。
目标板地址为 `192.168.1.63`；主机的有线和无线接口分别为
`192.168.1.88`、`192.168.1.47`。到目标的邻居解析失败，`ping` 和 TCP/22
均不可达，`adb devices -l` 为空，`adb connect 192.168.1.63:5555` 返回
`No route to host`。没有板端 RockIVA 输出，因此不能填写检测、跟踪、释放或
资源指标。

静态准备已确认：

- `aarch64-rockchip1240-linux-gnu-gcc` 可用，探针产物为 AArch64 ELF；
- staging 中存在 `librockiva.so`、`librknnrt.so` 和
  `iva_object_detection_v3_pfp.data`，`oem` 输出也包含对应文件；
- 当前未确认板端实际模型可加载、NPU 核心掩码、输入方式或许可证状态。

## 板卡恢复后的最小流程

```sh
SDK=/home/Tiger/Documents/code/linux/rv1126b_sdk/rv1126b_linux_ipc_xiaoyu
PROBE=$SDK/custom_part/media_engine/tests/board/rockiva_probe
BOARD=192.168.1.63
ADB=$BOARD:5555

make -C "$PROBE" SDK_ROOT="$SDK"
adb connect "$ADB"
adb -s "$ADB" get-state
adb -s "$ADB" push "$PROBE/rockiva_probe" /tmp/rockiva_probe
adb -s "$ADB" push "$PROBE/run_probe.sh" /tmp/run_probe.sh
adb -s "$ADB" push /tmp/rockiva-640x360-30.nv12 /tmp/input.nv12
adb -s "$ADB" shell 'chmod 755 /tmp/rockiva_probe /tmp/run_probe.sh'
adb -s "$ADB" shell '
  uname -a
  tr -d "\000" </proc/device-tree/model; echo
  cat /etc/os-release
  ls -l /oem/usr/lib/librockiva.so /oem/usr/lib/librknnrt.so \
        /oem/usr/lib/iva_object_detection_v3_pfp.data
  MODEL_PATH=/oem/usr/lib ROCKIVA_LIB_DIR=/oem/usr/lib INPUT=/tmp/input.nv12 \
  WIDTH=640 HEIGHT=360 FRAMES=30 FPS=10 MODEL=pfp /tmp/run_probe.sh
' 2>&1 | tee t1-rockiva-board.log
```

输入文件必须是严格的 `width * height * 3 / 2` 字节；默认 30 帧的
`640x360` 文件为 `10,368,000` 字节。CPU 地址探针成功后，仍必须用真实采集
DMA-BUF 重复生命周期测试，并记录 `objId/state`、推理时延、NPU/CPU/内存、
温度、丢帧和主视频不受影响的证据，才可解除 T3。
