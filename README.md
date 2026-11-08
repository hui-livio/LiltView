# LiltView

轻量、流畅的图片浏览器，首发目标平台为 Arch Linux。

已支持 JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放和 TIFF 首页的后台加载与显示，可通过命令行、打开菜单 / Ctrl+O 或拖放进入。支持适应窗口、EXIF 方向、中英双语错误提示，以及原有的系统顶栏菜单、主题和设置。渲染为独立模块：Linux 使用 OpenGL 3.3，接口为后续 Win32 + DX11 保持一致。

位图文件上限 64 MiB、原始 RGBA 上限 256 MiB，还须满足 CPU / GPU 预算；SVG 另限制为 1 MiB、8192 元素、128 层。透明图按 straight RGBA8 显示；SVG 为基础静态子集，文字、裁剪、滤镜和内嵌图片等暂不支持。普通 GIF 已支持循环、透明合成和按时长自动播放；动画 WebP / APNG、静态超大图、渐进预览、目录导航及 REGION 暂未实现。完整范围见[格式兼容表](docs/IMAGE_FORMATS.md)。

## 构建与检查

需要 CMake 3.30+、Ninja 以及支持所需 C23 / C++23 特性的工具链。x86 建议安装 NASM 以启用 libjpeg-turbo SIMD；没有 NASM 时上游回退标量构建。先初始化固定 submodule，CMake 只使用本地源码，不下载依赖：

```bash
git submodule update --init --recursive
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

显式 C++20 兼容模式使用 `compat20` preset，优化构建使用 `release`。启用第三方组件与 Catch2 使用 `dev`；源码统一位于 `third_party/`，编译库使用 Git submodule，纯头文件库直接保存源码。

libjpeg-turbo、libpng、zlib、libwebp、giflib、LibTIFF、PlutoSVG（嵌套 PlutoVG）、ImGui、spdlog、SDL3、Catch2 通过 submodule 固定提交。libjpeg-turbo 经独立 ExternalProject 构建并静态链接，不覆盖系统 JPEG 库。构建运行主程序：

```bash
git submodule update --init --recursive
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
./build/dev/app/liltview
./build/dev/app/liltview /path/to/photo.jpg
```

裁剪后的 SDL3 以 **静态库**链接到主程序，不链接或安装 `libSDL3.so`，不覆盖系统 SDL。Linux 仍使用系统 OpenGL、Fontconfig 和 X11 / Wayland 运行库；全局菜单默认开启，另需 GIO / GLib、Wayland 客户端和 `wayland-scanner`。可通过 `-DLILTVIEW_ENABLE_GLOBAL_MENU=OFF` 构建窗口内菜单版本，不依赖 Qt。可用 `--language zh-CN` 或 `--language en-US` 指定启动语言。程序随包使用 **HarmonyOS Sans SC Regular**，附带其独立许可证；字体约 7.88 MiB。

字体加载前校验随包文件，损坏时回退系统字体；没有中文字体时使用英文。命令行使用 CLI11，设置使用 iguana 读写 YAML，保存到 SDL 用户配置目录的 `settings.yaml`。只保留 YAML，不读取或迁移旧 INI；完整写入临时文件并检查刷新 / 关闭后再替换，失败会记录原因。

YAML 的 `log_level` 配置 spdlog 日志等级，支持 trace / debug / info / warn / error / critical / off。没有有效配置时，Debug 默认 debug，发布构建默认 warn；有效配置优先，重启生效。

开发构建首次接入桌面 / Dock 时执行以下命令，注册指向当前构建的用户级启动器和彩色图标，再重新启动 LiltView：

```bash
python3 scripts/register_desktop.py
```

应用 ID、桌面文件名和图标名统一为 `io.github.hui_livio.LiltView`。正式安装也会部署同名 `.desktop` 和图标资源。主窗口使用 256px 内嵌图标及 mipmap 缩小采样，避免原 64px 图标放大显示的模糊。

主题在“设置 → 外观”选择，默认跟随系统并响应系统主题变化；手动选择深色或浅色后保存到 YAML。

`app-release` preset 构建优化的主程序；安装、冒烟测试和完整参数见[目录与构建说明](docs/PROJECT_STRUCTURE.md)。

## 设计方向

- C23 核心负责图片解码、数据预读、后台任务和缓存。
- C++23 + Dear ImGui 负责界面与窗口行为；必要时显式提供 C++20 兼容构建。
- 使用 CMake、spdlog、Catch2 / CTest，窗口采用 SDL3 + OpenGL 3.3。
- 已有中英双语界面；更新检测优先使用 cpp-httplib，但暂不实现，当前不引入其网络依赖。
- 已接入 JPEG（JPG / JPEG）、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放和 TIFF 首页。AVIF、JPEG XL、HEIF / HEIC、APNG、动画 WebP、ICO / CUR、QOI、JPEG 2000、OpenEXR、相机 RAW / DNG，以及可配置宽高和布局的原始 RGB / 灰度、YUV、Bayer 输入均列为待开发，详见[格式支持计划](docs/FORMAT_ROADMAP.md)。
- 根据图片规模采用完整后显示、渐进更新或超大图分块处理。
- 动画规划覆盖普通尺寸动图；超大图规划只针对静态图，不考虑超大动图。
- 保持公共 C API 独立，为后续 Win32 界面适配保留空间。
- 不计划支持 CAD、PSD 等专用设计源文件。

## 文档

- [首版设计文档](docs/DESIGN-v0.1.md)
- [Core 动画会话与 GIF 播放](docs/CORE_ANIMATION-v0.2.md)
- [图片格式与解码实现](docs/IMAGE_FORMATS.md)
- [格式兼容支持待开发清单](docs/FORMAT_ROADMAP.md) · [原始 RGB / YUV / Bayer 输入方案](docs/RAW_PIXEL_INPUT.md)
- [性能测试方法与格式矩阵](benchmarks/TEST_PLAN.md) · [棋盘格随机色块输入](benchmarks/inputs/README.md)
- [跨平台渲染模块设计](docs/RENDERER-v0.1.md)
- [CLI11 命令行与 iguana YAML 配置](docs/CONFIGURATION.md)
- [工程落地与中英双语方案](docs/IMPLEMENTATION-v0.1.md)
- [Core API 设计](docs/CORE_API-v0.1.md) · [调用时序与生命周期契约](docs/CORE_RUNTIME-v0.1.md) · [公共 C 头文件](core/include/liltview/core.h)
- [Core 接口兼容性 Review](docs/CORE_INTERFACE_REVIEW-2026-11-08.md) · [格式兼容接口扩展方案（待开发）](docs/CORE_FORMAT_EXTENSION_PLAN.md)
- [项目目录与 CMake 构建](docs/PROJECT_STRUCTURE.md)
- [第三方源码与 submodule 管理](third_party/README.md)
- [默认鸿蒙字体与授权来源](assets/fonts/README.md)
- [项目审查与逐项需求验收](docs/PROJECT_REVIEW-2026-11-07.md)
- [当前彩色图标说明](docs/BRANDING-v0.2.md)
- [命名与首版图标记录](docs/BRANDING-v0.1.md)

## 图标

<img src="assets/icons/png/liltview-color-256.png" alt="LiltView 彩色应用图标" width="128" height="128">

- [SVG 源图](assets/icons/liltview-color.svg)
- [Windows ICO](assets/icons/liltview-color.ico)
- [小尺寸 SVG](assets/icons/liltview-color-small.svg)
- [PNG 尺寸集合](assets/icons/png/)

当前选用彩色版本，已用于主窗口与空状态画面。

## 许可证

Copyright 2026 LiltView contributors.

LiltView 采用 [Apache License 2.0](LICENSE)，SPDX 标识为 `Apache-2.0`。第三方依赖遵循各自的许可证。

JPEG 解码使用 libjpeg-turbo。This software is based in part on the work of the Independent JPEG Group. 第三方署名见 [NOTICE](NOTICE)。

HarmonyOS Sans SC 字体版权归 Huawei Device Co., Ltd.，使用[其原始许可协议](assets/fonts/LICENSE.HarmonyOS-Sans.txt)，不适用项目代码的 Apache-2.0。
