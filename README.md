# LiltView

轻量桌面图片浏览器，首发目标平台为 Arch Linux。C23 核心负责后台解码，C++23 / SDL3 / Dear ImGui 提供界面，Linux 使用 OpenGL 3.3。

<img src="assets/icons/png/liltview-color-256.png" alt="LiltView 彩色应用图标" width="96" height="96">

## 当前状态

以 2026-11-10 工作区源码为基线，功能按实际子集声明：

- **已实现**：JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放、TIFF 首页；命令行 / 菜单 / Ctrl+O / 拖放打开，同目录自然排序导航（裸 `←` / `→`、菜单、首尾禁用的边缘悬浮按钮）、适应窗口、100%、缩放平移、`I` 图片信息面板、全屏、中英、主题与窗口外观设置。
- **部分实现**：静态大图已有一次原始分辨率解码、行带显示、tile / LOD 场景缓存和 GPU 恢复；首次缓存生成、冷区域响应及强缩小质量仍需优化。已有 16K / 32K 回归用例，实际限制由格式和资源预算决定。
- **已接入相邻预读**：静态邻居复用 Core 内存内容缓存；普通 GIF 已实现按内容识别的会话 / 首帧预热，切换前重新核验版本，再按源时长继续播放。后台不上传 GPU、当前图片优先；第五轮最终验证记录见[开发计划](docs/DEVELOPMENT_PLAN.md)。
- **待实现**：动画手动控制、扩展格式 / 原始像素、ICC / HDR、D3D11、Windows 完整桌面适配与实机验收、更新检测。

静态图片加载完成后，缩放和平移复用已生成内容，不重新解码源文件。app 场景缓存与 Core 跨请求内存缓存分别有界管理；Core 磁盘内容缓存和通用完整动画缓存仍未实现。完整状态、`Shift+方向键` 平移及其他交互见[产品与设计](docs/PRODUCT_DESIGN.md)，格式边界见[兼容表](docs/IMAGE_FORMATS.md)。

## 构建与运行

需要 CMake 3.30+、Ninja，以及满足项目特性检查的工具链：Linux 使用 C23 / C++23；MSVC 使用 C17 兼容实现 / C++23。先初始化固定依赖；CMake 只使用本地源码，不自动下载：

```bash
git submodule update --init --recursive
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/app/liltview /path/to/image.png
```

`dev` 构建应用与单元测试，`app-release` 构建发布应用；`debug` / `release` 默认仅构建核心及相应检查，`compat20` 为显式 C++20 核心兼容配置。可选桌面 / 大图回归、C++20 应用和依赖要求见[架构文档](docs/ARCHITECTURE.md)。x86 建议安装 NASM 以启用 JPEG SIMD。

### Windows / MSVC + Ninja 预设

新增的 `msvc-dev` / `msvc-release` 使用 x64 MSVC (`cl.exe`) 和 Ninja，分别继承 `dev` / `app-release` 的构建选项，输出到 `build/msvc-dev` / `build/msvc-release`，不与原有工具链混用构建缓存。

命令行需先打开 **x64 Native Tools Command Prompt for VS** 或初始化 x64 Developer PowerShell，并确保 CMake 3.30+、Ninja 在 `PATH` 中。预设不会自行调用 `VsDevCmd.bat`；支持 CMake Presets 的 IDE 可使用预设中的 `external` 架构 / 工具集信息初始化 MSVC 环境。

```powershell
# 开发：Debug 应用 + 单元测试
cmake --preset msvc-dev
cmake --build --preset msvc-dev
ctest --preset msvc-dev

# 发布：Release 应用，不构建测试
cmake --preset msvc-release
cmake --build --preset msvc-release
```

VS 2022 / VS 2026 共用这两个预设，由当前 Developer Shell 或 IDE 选择工具链，不硬编码 VS 安装目录。更换 VS 工具链应使用独立构建目录，避免复用另一个编译器的缓存，例如：

```powershell
# 在对应 VS 的 x64 Developer Shell 中执行
cmake --preset msvc-dev -B build/msvc-dev-vs2022
cmake --build build/msvc-dev-vs2022
ctest --test-dir build/msvc-dev-vs2022 --output-on-failure
# VS 2026 可对应使用 build/msvc-dev-vs2026
```

MSVC 的 Core 明确使用 `/std:c17`、`/experimental:c11atomics`，并只对项目自有 C 目标启用私有 C23 写法兼容头；CMake 实际编译 / 链接检查 C11 原子与线程支持，不把 MSVC 声明为完整 C23 编译器。其他工具链保留原 C23 检查；公共 C ABI 不变。Windows 文件适配使用 UTF-8 到 UTF-16 的内部转换和 Win32 文件版本信息，tile 缓存使用删除即回收的临时文件和独立偏移读取。

**当前边界：Windows 使用现有 SDL3 + OpenGL3 后端，D3D11 尚未实现，完整桌面与大图实机验收仍待完成。** UTF-16 Core 路径输入仍返回 `LV_UNSUPPORTED`；Windows 调用方应传绝对 UTF-8 路径。两项依赖 POSIX 特有“目录符号链接后接 `..`”解析语义的回归在 Windows 上明确跳过，其余常规测试保持启用。

CLI11 解析命令行，iguana 读写 `settings.yaml`；可用 `--language zh-CN`、`--language en-US` 或 `--config-dir PATH`，完整参数见[配置说明](docs/CONFIGURATION.md)。

Linux 使用静态 SDL3 和 codec，仍需系统 OpenGL、Fontconfig 及窗口运行库；默认全局菜单另需 GIO / GLib 和 `wayland-scanner`。`-DLILTVIEW_ENABLE_GLOBAL_MENU=OFF` 可构建窗口内菜单版本。开发桌面注册：

```bash
python3 scripts/register_desktop.py
```

应用 ID 为 `io.github.hui_livio.LiltView`。安装 / pacman / paru 部署见[打包说明](packaging/README.md)和[Arch 指南](packaging/arch/README.md)。

## 文档入口

主文档各自维护产品和开发视角；细节只在对应参考中维护，避免重复状态与过时测试计数。

| 文档 | 内容 |
| --- | --- |
| [产品与设计](docs/PRODUCT_DESIGN.md) | 定位、界面、交互、功能状态与建议优先级 |
| [开发架构](docs/ARCHITECTURE.md) | 模块、依赖、数据流、线程、预算、构建与验证 |
| [Core 契约](docs/CORE_CONTRACTS.md) | ABI、请求 / 动画、事件、引用、取消和关闭 |
| [渲染与大图](docs/RENDERING.md) | 当前场景缓存、清晰度、上传契约与性能复现 |
| [格式兼容表](docs/IMAGE_FORMATS.md) | 已支持的具体子集和限制 |
| [命令行与配置](docs/CONFIGURATION.md) | 参数、YAML、默认值和保存语义 |
| [开发计划](docs/DEVELOPMENT_PLAN.md) | 未完成工作、原始像素 / 接口扩展和验收条件 |

配套说明：[性能测试方法](benchmarks/TEST_PLAN.md)、[输入样本](benchmarks/inputs/README.md)、[第三方依赖](third_party/README.md)、[字体来源](assets/fonts/README.md)。历史设计、阶段审查与零散待办已合并；后续直接更新上述文档。

## 许可证

Copyright 2026 LiltView contributors. 项目采用 [Apache License 2.0](LICENSE)，第三方组件遵循各自许可证，署名见 [NOTICE](NOTICE)。

JPEG 使用 libjpeg-turbo。This software is based in part on the work of the Independent JPEG Group.

随包 HarmonyOS Sans SC 字体版权归 Huawei Device Co., Ltd.，适用[字体原始许可](assets/fonts/LICENSE.HarmonyOS-Sans.txt)。
