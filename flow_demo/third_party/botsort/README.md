# vendored BoT-SORT core

来源：`wvp-ssv` commit `e542950714479ae423644310794cdb41b6608adf`，文件来自 `gst/ssv-track/core/botsort/`。

本次只复制不含 OpenCV GMC 的 Kalman、matching、LAPJV、tracker、types 和 coordinates 文件；`botsort_gmc.*` 也保留为可选接口，但 Demo 首版显式使用 `GmcMethod::kNone`，构建时关闭 OpenCV。LAPJV 的 BSD-2-Clause 许可证保留在 `LAPJV_LICENSE.txt`。该副本不依赖源工作树。
