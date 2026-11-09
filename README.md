# LiltView

轻量桌面图片浏览器，首发目标平台为 Arch Linux。C23 核心负责后台解码，C++23 / SDL3 / Dear ImGui 提供界面，Linux 使用 OpenGL 3.3。

<img src="assets/icons/png/liltview-color-256.png" alt="LiltView 彩色应用图标" width="96" height="96">

## 当前状态

以 2026-11-09 工作区源码为基线，功能按实际子集声明：

- **已实现**：JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放、TIFF 首页；命令行 / 菜单 / Ctrl+O / 拖放打开，同目录自然排序导航（左右键 / 菜单 / 边缘悬浮按钮）、适应窗口、100%、缩放平移、`I` 图片信息面板、全屏、中英、主题与窗口外观设置。
- **部分实现**：静态大图已有一次原始分辨率解码、行带显示、tile / LOD 场景缓存和 GPU 恢复；首次缓存生成、冷区域响应及强缩小质量仍需优化。已有 16K / 32K 回归用例，实际限制由格式和资源预算决定。
- **已接入静态预读**：在预算内预读目录两侧邻居并复用 Core 内容缓存，后台不上传 GPU；当前图片优先，修改 / 删除会重新核验。GIF 首帧与会话预热仍在开发。
- **待实现**：动画手动控制、扩展格式 / 原始像素、ICC / HDR、Windows / D3D11、更新检测。

静态图片加载完成后，缩放和平移复用已生成内容，不重新解码源文件。app 当前图缓存已经实现，与仍未实现的 core 跨请求缓存不同。完整状态和交互见[产品与设计](docs/PRODUCT_DESIGN.md)，格式边界见[兼容表](docs/IMAGE_FORMATS.md)。

## 构建与运行

需要 CMake 3.30+、Ninja，以及满足项目特性检查的 C23 / C++23 工具链。先初始化固定依赖；CMake 只使用本地源码，不自动下载：

```bash
git submodule update --init --recursive
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/app/liltview /path/to/image.png
```

`dev` 构建应用与单元测试，`app-release` 构建发布应用；`debug` / `release` 默认仅构建核心及相应检查，`compat20` 为显式 C++20 核心兼容配置。可选桌面 / 大图回归、C++20 应用和依赖要求见[架构文档](docs/ARCHITECTURE.md)。x86 建议安装 NASM 以启用 JPEG SIMD。

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
