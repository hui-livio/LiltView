# RAW / DNG 依赖与许可证门禁

RAW / DNG 首版先固定 LibRaw 源码和发布边界，不使用系统 libraw 作为正式构建依赖。当前只完成依赖登记和范围审查，尚未接入 Core 或应用支持。

## 固定源码

| 组件 | 版本 | 固定提交 | 接入方式 |
| --- | --- | --- | --- |
| LibRaw | 0.22.3 | `cf02d66eb5c19d4e46df7aee0582e47a4918509a` | `third_party/libraw` submodule |

LibRaw 上游没有受官方支持的 CMake 构建入口，正式集成需要固定 autotools 构建参数，或在主工程中提供可复核的静态源码目标。构建必须关闭示例、命令行工具、OpenMP 和不需要的外部处理后端，先保留 C API、DNG 解析和首版样本所需的解码路径。系统 LCMS、系统 JPEG 和系统 LibRaw 不能通过探测自动混入正式目标。

## 许可证与发行义务

LibRaw 主体采用 LGPL-2.1-only 或 CDDL-1.0 双许可证。源码树还包含 DCB、FBDD、X3F 等组件的独立许可证，以及 Adobe DNG SDK 相关 MIT 许可文本。正式构建前必须逐文件核对实际编译对象、许可证文本和静态链接义务，并把 LGPL、CDDL、BSD-like 和 MIT notices 纳入发行包检查。

## 首版候选范围

首版只考虑 DNG 和明确列入白名单的相机 RAW 样本。优先读取合法的内嵌 JPEG 预览，预览不存在时才使用 LibRaw 的 8-bit RGB 处理结果。输出统一为 RGBA8，支持 NATIVE / FIT，不支持裸 `.raw` 布局猜测、REGION、HDR、原生高位深、任意未知相机、完整 ICC 管理或批量多帧。

每个进入白名单的样本必须记录相机型号、容器、主图尺寸、预览身份、CFA / active area 元数据和资源预算。仓库当前没有可提交的真实 RAW / DNG 样本，不能在取得可再分发样本和许可证确认前进入 Core 解码提交。

系统 LibRaw 只能用于 API 探针和样本审查，不能作为正式构建成功证据。下一批应先提交可再分发 fixture 清单和静态构建探针，再决定是否进入 Core。
