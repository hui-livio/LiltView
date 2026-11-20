# Linux 依赖迁移开发计划

本文冻结 Linux 依赖的迁移顺序、平台边界、自动获取规则和每批验收条件。当前仓库仍按现有 CMake 逻辑使用本地子模块，本文是后续实施计划，不改变当前构建行为。

## 目标与边界

- Linux 配置时优先查找系统已安装的开发包、CMake package 和 pkg-config 模块。
- 系统缺失时，由 CMake 在构建目录内下载固定版本源码并构建，下载内容不写入 `third_party/`，不修改 Git submodule 工作树。
- 依赖必须在配置 / 构建阶段准备完成。`cmake --install` 只负责安装已经构建好的目标，不能在链接完成后补下载依赖。
- 下载源、版本、提交或归档哈希统一来自 `third_party/dependencies.json` 或新的依赖清单字段，禁止使用未固定的分支、浮动标签或“最新版本”。
- Win32 保持当前本地子模块、静态目标、编译选项和依赖路径。Linux 条件只能通过明确的 `CMAKE_SYSTEM_NAME STREQUAL "Linux"` 或等价平台判断进入。
- HEIF / libheif / libde265 按现有决定暂停，本轮只保留审计记录，不迁移其依赖方案。
- 纯头文件库、项目内改编代码和平台协议数据继续按各自现有规则管理，除非后续批次单独冻结迁移范围。

## 目标构建模型

Linux 依赖选择按以下顺序执行。

1. 对可由系统提供的库执行版本和目标检查，优先使用导出的 CMake target，必要时使用 pkg-config。
2. 系统库满足首版最低版本、组件和 ABI 要求时，映射到项目现有 `LiltView::*` 目标，调用方不感知来源变化。
3. 系统库缺失、版本不足或目标不完整时，使用 `FetchContent` 或 `ExternalProject` 在 `${CMAKE_BINARY_DIR}/_deps` 下获取固定源码，并生成与当前调用方兼容的静态目标。
4. 下载失败时保留清晰的依赖名称、版本、来源和离线处理提示。配置不得悄悄切换到不兼容的系统库。
5. 安装阶段只安装项目产物、必要的运行时资源和现有许可证 notices，不把临时源码目录复制到安装树。

系统探测和 fallback 逻辑必须集中在可复用的 CMake helper 中。codec、应用库和测试库都通过同一层暴露项目目标，避免在各个 `CMakeLists.txt` 中重复判断。

## 分批实施计划

### D0 计划基线

提交本文件，冻结 Linux / Win32 边界、依赖选择顺序、下载目录、版本锁定、离线行为、许可证和提交验收规则。

验收条件是文档与现有 CMake 行为一致，工作区无未说明修改。

### D1 Linux 依赖查找与 fallback 层

新增统一 helper，至少覆盖以下能力。

- Linux 系统 CMake package、pkg-config 和版本范围检查。
- 固定版本源码的 FetchContent / ExternalProject fallback。
- `${CMAKE_BINARY_DIR}/_deps` 目录隔离，禁止污染 Git submodule。
- 可选的离线模式和清晰的网络失败信息。
- 统一的 imported / alias target 名称，保持现有调用方链接接口。
- 配置摘要显示每个依赖最终使用 system 或 fetched，以及版本来源。

验收条件是一个最小虚拟依赖可分别走 system、fallback 和离线失败路径，Win32 配置与现有逻辑保持一致。

### D2 Linux 基础依赖

按风险从低到高迁移以下依赖。

| 依赖 | 系统探测重点 | fallback 重点 |
| --- | --- | --- |
| SDL3 | SDL3 CMake package / pkg-config，静态链接选项和现有平台驱动配置 | 固定 SDL 版本，保留当前裁剪项 |
| spdlog | `spdlog::spdlog` 或 pkg-config，头文件和编译库 ABI | 固定版本、关闭示例 / 测试 |
| zlib | `ZLIB::ZLIB` | 固定 zlib 静态目标 |
| libpng | `PNG::PNG` 及 zlib 配对 | 固定 libpng / zlib 组合 |
| libwebp | decoder 目标和头文件版本 | 只构建应用所需 decoder |
| libtiff | TIFF 目标、压缩和多页 API | 保留当前裁剪构建 |
| giflib | giflib 头文件和解码库 | 保留解码器最小构建 |
| LZ4 | `lz4` / pkg-config 或静态库 | 保留项目使用的 `lz4.c` / `xxhash.c` 子集 |
| OpenJPEG | `openjp2` target、版本和 C API | 关闭工具、测试和无关组件 |
| libjpeg-turbo | jpeg-static target、SIMD 能力和 NASM 情况 | 保留现有 ExternalProject 静态构建 |

D2 每项依赖至少包含 system-only、fallback-only 和全量组合配置验证。每完成一组依赖，单独提交、review，并运行对应 Core / app / integration 测试。

### D3 Linux 高级格式依赖

在 D2 稳定后迁移以下依赖。

- libavif 与 dav1d，固定 decoder 组合和静态链接边界。
- JPEG XL 及 Highway、Brotli 等传递依赖，明确系统组件兼容性。
- OpenEXR 与 Imath，保持 OpenEXRCore C API 和现有裁剪选项。
- PlutoSVG / PlutoVG，确认系统包缺失时的嵌套源码获取和 notices。
- LibRaw，保留 UNIX 路径、许可证 notices 和现有 RAW 首版范围。

HEIF / libheif / libde265 不进入 D3 实现。

### D4 构建、安装与回归验收

每个迁移批次都必须验证以下矩阵。

- 所有目标依赖均由系统提供。
- 至少一个系统依赖缺失，自动下载并完成构建。
- 下载目录不产生 `third_party/`、`.gitmodules` 或 gitlink 修改。
- 禁止网络或清空依赖缓存时，错误信息可定位到具体依赖和版本。
- `LILTVIEW_BUILD_UNIT_TESTS=ON`、C / C++ ABI contract、全量 CTest 通过。
- Linux `cmake --install` 安装树、运行时加载和许可证 notices 正确。
- system 与 fallback 两种来源的目标名、编译宏、ABI 和运行行为一致。
- Win32 预设、现有子模块路径和编译选项不发生行为变化。

### D5 文档与最终审查

更新构建、第三方依赖、打包和发布文档，记录以下内容。

- Linux 系统包的最低版本和推荐安装命令。
- 网络可用、缓存复用和离线构建方式。
- 每个依赖的 system / fetched 结果和固定来源。
- 安装树中的许可证与 notices。
- Linux 与 Win32 的构建矩阵及尚未验证的平台边界。

## 提交与 review 规则

每批使用独立 Git 提交，提交前检查 `git diff --check`、构建目录来源摘要和工作区状态。建议主题如下。

- `docs: plan linux dependency resolution`
- `cmake: add linux system dependency fallback`
- `cmake: migrate linux base dependencies`
- `cmake: migrate linux advanced dependencies`
- `test: verify linux dependency source matrix`
- `docs: document linux dependency installation`

每批提交后先做代码 review，再执行该批 targeted tests，最后执行全量 Linux 验收。未通过验收的依赖不能在兼容表、发布文档或安装说明中标记为迁移完成。
