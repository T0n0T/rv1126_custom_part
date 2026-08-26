# T0 基线与回滚记录

状态：T0 主机基线完成，真板/WVP 运行态验证保留到后续门禁
记录日期：2026-08-26（Asia/Shanghai）
基线提交：`955176d010fcd1d7e6084f07b4c923fff382dc0a`

## 范围

本记录只确认当前 `media_engine`、`gb28181_daemon` 和规划文档的起始状态。没有在 T0 引入 RockIVA、RKNN、分析分支、事件日志或 GB28181 Alarm 生产代码。

初始提交纳入了媒体/daemon 源码、配置、测试和人流规划文档；构建产物、二进制、缓存、代码库索引和 GStreamer 大型依赖被排除。当前工作树中其他未跟踪文件保持不动。

## 当前功能基线

| 项目 | 当前证据 | 结论 |
| --- | --- | --- |
| media_engine 采集/编码/IPC | `media_engine/README.md`、源码和宿主测试 | 已有采集、编码、动态 RTP 分支和 Unix socket 控制边界 |
| gb28181_daemon 信令 | `gb28181_daemon/README.md`、Go 测试 | 已有 REGISTER、Keepalive、Catalog/DeviceInfo、INVITE/BYE 会话骨架 |
| 分析/RockIVA/RKNN | `rg` 检索 `media_engine` 和 `gb28181_daemon` 无匹配 | 当前没有端侧分析实现 |
| GB28181 Alarm | `rg` 检索 Alarm 字段和 `CmdType` 无匹配 | 当前没有 Alarm XML 上送实现 |
| 分析关闭路径 | 当前配置没有 `analytics` 键或分析分支 | 现有媒体行为等价于分析关闭；不需要改动即可回滚 |
| daemon 测试配置 | `gb28181_daemon/configs/gb28181.test.json` 使用 `media.mode=none`、`simulate=true` | 主机联调默认不启动真实媒体引擎 |

现阶段不能把跟踪、人数、告警或图片能力归因于现有基线；这些均属于 T1 之后的新能力。

## 主机环境与命令

| 项目 | 结果 |
| --- | --- |
| 主机 | `Linux tkotk 6.18.44-1-lts x86_64` |
| Go | `go1.27.0-X:nodwarf5 linux/amd64` |
| C 编译器 | GCC 16.2.1 |
| Git HEAD | `955176d` |
| daemon 单测 | `make -C gb28181_daemon test`：通过 |
| daemon 静态检查 | `make -C gb28181_daemon vet`：通过 |
| media_engine 宿主测试 | `make -C media_engine/tests`：session/config 全部通过 |

主机测试只证明现有确定性模块没有被规划提交破坏，不能替代 RV1126B NPU、摄像头、RTP 或 WVP 运行态验收。

## 未验证项与边界

- 当前主机 PATH 中没有 `aarch64-rockchip1240-linux-gnu-gcc` 或 `arm-rockchip1240-linux-gnueabihf-gcc`，本轮未执行目标交叉构建。
- `/home/Tiger/Documents/code/web/wvp-baseline` 和 `baseline.sh` 存在，但现有联调记录显示容器未运行；本轮未启动外部 WVP/ZLMediaKit，也未宣称当前 WVP 互操作通过。
- 当前会话没有可核对的板卡、固件、RockIVA license/model、NPU 负载或 24 小时稳定性数据；T1 必须在真板上重新采集。
- `docs/ipc_interop.md` 中的历史联调结果作为背景参考，不当作本次运行态证据。

## 回滚判定

T0 的回滚方式是保持初始提交中的现有媒体/daemon 配置与路径不变，不启动任何分析分支。后续引入分析配置时，出厂默认必须使用 `analytics.enabled=false`；关闭后应回到本记录所描述的媒体基线。

## T0 结论

主机基线和回滚边界已记录，T0 可以标记完成。T1（真板 RockIVA 探针）与 T2（观察结果/配置契约）现在解除 T0 阻塞，可并行进入实施；T1 的实测结果仍是进入产品化分析分支的硬门禁。
