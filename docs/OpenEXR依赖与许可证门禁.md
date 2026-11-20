# OpenEXR 依赖与许可证门禁

OpenEXR 首版使用仓库内固定源码，不使用系统 OpenEXR、Imath、libdeflate、zstd 或 OpenJPH，也不允许 CMake 在配置阶段联网下载依赖。

## 固定源码

| 组件 | 版本 | 固定提交 | 接入方式 |
| --- | --- | --- | --- |
| OpenEXR | v3.5.4 | `86687e5f34e3a164820c5e280259550e3ca87c92` | `third_party/openexr` submodule |
| Imath | v3.2.3 | `5f27ba266d3ea1565e912570c30b5eafc89959f1` | `third_party/Imath` submodule |
| OpenEXR 内置 deflate | 随 OpenEXR 固定 | OpenEXR 源码内 `external/deflate` | 仅构建解码所需静态代码 |
| OpenEXR 内置 zstd | 随 OpenEXR 固定 | OpenEXR 源码内 `external/zstd` | 仅构建解码所需静态代码 |
| OpenEXR 内置 OpenJPH | 随 OpenEXR 固定 | OpenEXR 源码内 `external/OpenJPH` | 仅构建解码所需静态代码 |

构建裁剪关闭 OpenEXR 工具、示例、Python、测试、文档和安装目标，仅保留 `OpenEXR::OpenEXRCore` 解码库及其必要的 `Iex`、`IlmThread`、`OpenEXRCore` 和 Imath 目标。构建配置必须显式关闭 shared，并强制使用内部 deflate、zstd 和 OpenJPH。Imath 在主工程中先由固定 submodule 建立目标，再作为 OpenEXR 的内部目标使用，避免 FetchContent。

## 许可证

OpenEXR 与 Imath 源码使用 BSD-3-Clause。内置 deflate、zstd 和 OpenJPH 的原始许可证随 OpenEXR 源码保留，发行包需要同时带上对应 LICENSE / COPYING 文件并纳入 notices 检查。应用集成提交必须安装 OpenEXR、Imath 和内置组件的许可文件，不能只安装顶层 OpenEXR LICENSE.md。

## 首版解码边界

首版只接受 single-part、flat、scanline 图像，RGB 或 RGBA 通道，通道类型为 HALF，采样为 1。data window 可以有负原点，输出画布按 data window 的宽高规范化为 RGBA8。display window 只用于边界校验和信息记录。

首版拒绝 deep、multi-part、tiled、任意未知通道、FLOAT / UINT、HDR 原生输出、ICC 颜色管理和 REGION 解码。half 样本先转有限的 float，使用固定的 SDR 映射和 alpha 规则。NaN 与负无穷映射为 0，正无穷按映射上限处理，结果再量化到 8-bit。

依赖门禁完成后，才能提交 Core 解码器。系统库探针只能用于审查和生成 fixture，不能把系统 OpenEXR 标记为应用支持或发行依赖。
