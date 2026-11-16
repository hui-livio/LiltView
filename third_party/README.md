# 第三方源码管理

统一放在 `third_party/`，按构建形式分为两类：

- **需要编译的库**：Git submodule，主仓库的 gitlink 固定具体提交。
- **纯头文件库**：直接提交所需头文件，保留原许可证、上游版本 / 提交与来源记录，不额外创建 submodule。

spdlog 本项目采用编译库形式；因此与 ImGui、SDL3、Catch2 一样使用 submodule。后续 cpp-httplib 为直接保存头文件的候选，但更新模块延期，本轮不下载或构建它。字体是资源，保存在 `assets/fonts/` 并附原许可和校验信息。

## 当前声明

| 路径 | 版本基线 | 固定提交 | 接入形式 |
| --- | --- | --- | --- |
| `libjpeg-turbo/` | 3.1.2 | `4e151a4ad91001b3aa8c2ece2205c15f487ce320` | submodule、ExternalProject 静态库 |
| `libpng/` | v1.6.58 | `3061454d980de7d53608f594194cfac722721d2a` | submodule、静态库 |
| `zlib/` | v1.3.2 | `da607da739fa6047df13e66a2af6b8bec7c2a498` | submodule、静态库 |
| `libwebp/` | v1.6.0 | `4fa21912338357f89e4fd51cf2368325b59e9bd9` | submodule、静态库 |
| `plutosvg/` | v0.0.8 | `fd8a080b3d0bccc41f1ae708d3785e7f08398a58` | submodule、静态库 |
| `plutosvg/plutovg/` | v1.3.3 | `bbd91f0d06a71491691b36330f29dffa4af87ccf` | PlutoSVG 嵌套 submodule、静态库 |
| `imgui/` | v1.92.0 | `85b2fe8486190fa9326565a2fb5fccb6caea4396` | submodule |
| `spdlog/` | v1.15.3 | `6fa36017cfd5731d617e1a934f0e5ea9c4445b13` | submodule、编译库 |
| `Catch2/` | v3.8.1 | `2b60af89e23d28eefc081bc930831ee9d45ea58b` | submodule、仅测试 |
| `SDL/` | release-3.4.0 | `a962f40bbba175e9716557a25d5d7965f134a3d3` | submodule、窗口 / 输入 |
| `giflib/` | 6.1.3 | `edff4aed17f857442ab0cac31566572ba08f93d3` | submodule、静态解码库，编码库仅测试 |
| `libtiff/` | v4.7.2 | `d01a94be176f5f6a87f7ee1c0b32e65416aa2b4d` | submodule、裁剪版静态库 |
| `CLI11/` | v2.5.0 | `4160d259d961cd393fd8d67590a8c7d210207348` | 纯头文件源码直接保存 |
| `lz4/` | v1.10.0 | `ebb370ca83af193212df4dcbadcc5d87bc0de2f0` | submodule、仅 lz4.c / xxhash.c 静态库 |
| `dav1d-ref/` | 引用生命周期受控子集 | `39834d7f71c85a9c555f2ddd7a7add6d671750de` | C11 头文件抽取 / 改编，BSD-2-Clause；非完整 decoder |
| `iguana/` | 2026-08-12 快照 | `2c05e011942c652d020a6782d026c2378c42008a` | YAML 及传递头文件直接保存 |

初始化脚本读取 `dependencies.json`，其发布页字段记录版本来源。基线不是“最新版本”声明，升级时需同时审阅 manifest 和 gitlink 改动。

libjpeg-turbo 使用本地 ExternalProject 独立构建 jpeg-static，因为上游不支持 add_subdirectory。关闭 shared、TurboJPEG API、tools 和 tests；安装附 LICENSE.md / README.ijg，不安装 JPEG 动态库。x86 SIMD 需要 NASM，没有时上游回退标量。

PNG 使用本地 libpng / zlib 静态 target；WebP 应用仅链接 decoder，测试才构建编码器。
PlutoSVG / PlutoVG 关闭示例、FreeType 集成和系统字体缓存读取，按固定嵌套源码构建，不探测系统 PlutoVG。安装同时保留 PlutoVG bundled stb 的 MIT 署名、FreeType FTL 和原始版权声明，汇总在 `PlutoVG-NOTICES.txt`。
SVG 当前只使用上述 PlutoSVG / PlutoVG 基础静态 profile，复杂 SVG 不支持。没有新增 SVG 后端、Rust 工具链或浏览器运行时；后续选型受轻量 C/C++、无重型框架及无 V8 约束，见[技术决策](../docs/开发计划与未实现设计.md#7-复杂-svg-范围与技术决策)。
编译型依赖保持上游源码原样；纯头文件 iguana 的受控补丁见下文及其来源清单。BMP 由项目 C23 实现。功能边界与验证见[格式说明](../docs/图片格式兼容表.md)。

## 首次注册

当前声明 12 个直接 submodule 及 PlutoSVG 嵌套的 PlutoVG；版本以 `dependencies.json`、gitlink 和实际 checkout 为准，使用 `python3 scripts/init_submodules.py --check` 检查。SDL3 设置 `SDL_SHARED=OFF`、`SDL_STATIC=ON`，应用明确链接 `SDL3::SDL3-static`。关闭音频、相机、手柄、传感器等未用子系统，保留窗口、输入、SDL_Render（含平台驱动）和 Linux 的 X11 / Wayland。不安装裁剪版 SDL 动态库，也不选择系统 SDL。平台图形驱动由 SDL 在运行时选择与加载，应用不直接链接 OpenGL；平台运行库仍按动态方式使用，Linux 还依赖 Fontconfig 等桌面组件，这不是完整静态应用。

`plasma-wayland-protocols/` 保存 KDE appmenu 协议描述、原 MIT 许可和带 SHA-256 的来源清单。它是构建时生成 Wayland C 接口所需的协议数据，不是完整第三方库；没有复制 Qt 实现或链接 Qt。生成工具 wayland-scanner、GIO / GLib、Wayland 和 X11 由系统开发环境提供。

下列首次注册流程用于维护或重新建立依赖。仅有 `.gitmodules` 不等于 submodule 已完成接入，必须同时记录 gitlink。

在具备网络和可写 Git 元数据的终端执行：

```bash
python3 scripts/init_submodules.py
python3 scripts/init_submodules.py --check
git diff --cached --submodule
```

脚本会注册库、检出固定提交并暂存 `.gitmodules` 与 gitlink，不自动提交或推送。遇到依赖目录已有普通源码或子模块工作区有修改时停止，不覆盖它们。注册失败可能已完成前面的库；解决错误后可以重跑。

首次完成后，应将 gitlink 与 CMake / 文档 / 清单一并提交。之后其他开发者正常使用：

```bash
git submodule update --init --recursive
python3 scripts/init_submodules.py --check
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

通常不使用 `git submodule update --remote`，以免绕过主仓库锁定版本。CMake 只读取本地源码，不自动执行 Git、下载或修改依赖。

## 纯头文件库规则

直接保存到 `third_party/<name>/`，至少包含实际头文件、原始 LICENSE 和来源记录。后续在 manifest 的 `vendored_headers` 中登记版本、上游提交和保存文件的 SHA-256。若有额外实现 `.c/.cpp` 或要求某个宏生成实现，则按实际链接方式审查，不仅凭“单文件”归类。

头文件库在 CMake 中用 INTERFACE target 暴露包含路径；上游目录标记为 SYSTEM，避免将项目自身的严格告警施加给第三方代码。第三方文件遵循原许可证，不改标为本项目 Apache-2.0。

CLI11 保存未修改的 include/CLI 目录；iguana 保存 YAML reader / writer 和传递头文件，含其上游自带 frozen。iguana yaml_reader.hpp 有最小的类型 / 缩进校验补丁，差异保存在 patches/yaml-validation.patch，其余头文件保持上游原样。每库 source.json 记录来源、提交、补丁与逐文件 SHA-256。不调用两者上游 CMake，不编译其示例 / 测试。iguana 的 bundled 转换器各自原始许可另保存在 THIRD_PARTY_NOTICES.txt 和 LICENSE-Boost；安装包一并带上。

LZ4 用于应用内存图块缓存，与 PNG 解码继续使用的 zlib 分开。`dav1d-ref` 只抽取并改编固定提交的引用计数布局 / init / inc / dec，去掉 decoder、pool 和 allocator 依赖，使用标准 C11 原子；不是上游原样头文件。`source.json` 记录上下游 hash 与改动，安装包保留各自许可。
