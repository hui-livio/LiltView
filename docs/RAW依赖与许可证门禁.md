# RAW / DNG 依赖与许可证门禁

RAW / DNG 首版固定 LibRaw 源码和发布边界，不使用系统 libraw 作为正式构建依赖。当前已接入 Core 和应用，但支持范围仍限于两个真实样本覆盖的 DNG / Canon CR2 白名单。

## 固定源码

| 组件 | 版本 | 固定提交 | 接入方式 |
| --- | --- | --- | --- |
| LibRaw | 0.22.3 | `cf02d66eb5c19d4e46df7aee0582e47a4918509a` | `third_party/libraw` submodule |

LibRaw 上游没有受官方支持的 CMake 构建入口，正式集成需要固定 autotools 构建参数，或在主工程中提供可复核的静态源码目标。构建必须关闭示例、命令行工具、OpenMP 和不需要的外部处理后端，先保留 C API、DNG 解析和首版样本所需的解码路径。系统 LCMS、系统 JPEG 和系统 LibRaw 不能通过探测自动混入正式目标。

## 许可证与发行义务

LibRaw 主体采用 LGPL-2.1-only 或 CDDL-1.0 双许可证。源码树还包含 DCB、FBDD、X3F 等组件的独立许可证，以及 Adobe DNG SDK 相关 MIT 许可文本。正式构建前必须逐文件核对实际编译对象、许可证文本和静态链接义务，并把 LGPL、CDDL、BSD-like 和 MIT notices 纳入发行包检查。

## 首版候选范围

首版只支持 little-endian DNG 和 Canon CR2 白名单样本。当前直接使用 LibRaw 的 8-bit developed RGB 处理结果，不承诺内嵌 JPEG preview-first。输出统一为 RGBA8，支持 NATIVE / FIT，不支持裸 `.raw` 布局猜测、REGION、HDR、原生高位深、未列入白名单的相机、完整 ICC 管理或批量多帧。

每个进入白名单的样本必须记录相机型号、容器、主图尺寸、显影输出身份、CFA / active area 元数据和资源预算。当前样本和 SHA-256 记录在 `tests/fixtures/raw_sources.json`，正式测试不依赖网络。

系统 LibRaw 不能作为正式构建依赖。正式 UNIX 构建使用固定 submodule 的 Autotools 静态目标，应用 notices 安装 `LICENSE.LGPL`、`LICENSE.CDDL` 和 `COPYRIGHT`。Windows 当前保留 RAW 内容识别但不声明 LibRaw 解码可用，格式查询会隐藏 RAW 能力。
