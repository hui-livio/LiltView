# HEIF / HEIC 依赖与许可证门禁

HEIF 首版固定使用仓库内源码，不使用系统 libheif 或系统 libde265。当前阶段只完成依赖锁定与发行门禁，未把 HEIF / HEIC 标记为应用支持。

## 固定源码

| 组件 | 版本 | 固定提交 | 接入方式 |
| --- | --- | --- | --- |
| libheif | v1.23.7 | `12a259a5882dd99d45ec5fc77e296a3b620d1e2e` | `third_party/libheif` submodule |
| libde265 | v1.1.3 | `ba62bf4cfb3242f3bf0a45617ff09e35236e4d82` | `third_party/libde265` submodule |

libheif 构建必须关闭 shared、plugin loading、所有插件形式和未使用 codec，只保留 built-in libde265 HEVC decoder。libde265 构建关闭 shared、SDL、encoder、工具和示例，保留 decoder 与目标平台 SIMD。CMake 不得自动下载或选择系统 codec。

## 许可证与发行义务

libheif library 和 libde265 使用 LGPL-3.0-or-later。libheif 的示例与 C++ / Go wrapper 使用 MIT，但首版只链接 library。发行包必须同时提供 LGPL 文本、两套对应源码或完整对应源码获取方式、构建配置和替换链接所需信息。静态链接发布前必须完成 LGPL-3 合规审查，当前不提交应用入口和 Core decoder。

## 首版范围

计划支持单主图、HEVC、8-bit RGB / RGBA 输出。必须拒绝 sequence、multi-image、auxiliary / depth / thumbnail、10/12-bit、高动态范围、ICC 颜色管理和 REGION。主图选择、解码尺寸、方向 / 镜像变换和损坏输入分别验收。方向元数据若无法通过公开 C API 稳定报告，必须明确记录为已应用变换或未知，不能猜测原始方向。

系统 libheif / libde265 只能用于审查或生成 fixture，不能作为正式构建成功证据。下一批才进入 CMake target 绑定和 Core 内容识别。
