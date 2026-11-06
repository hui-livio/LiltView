# LiltView

轻量、流畅的图片浏览器，首发目标平台为 Arch Linux。

当前已初始化 CMake 工程、公共 C API 和核心配置初始化，尚未实现图片引擎与窗口程序。

## 构建与检查

需要 CMake 3.30+、Ninja 以及支持所需 C23 / C++23 特性的工具链。默认构建不下载第三方库：

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

显式 C++20 兼容模式使用 `compat20` preset，优化构建使用 `release`。启用第三方组件与 Catch2 使用 `dev`；源码统一位于 `third_party/`，编译库使用 Git submodule，纯头文件库直接保存源码。

ImGui、spdlog、Catch2 已注册为固定提交的 submodule，开发构建与 5 项测试已通过。正常克隆后执行 `git submodule update --init --recursive`，再用 `python3 scripts/init_submodules.py --check` 校验。详见 [第三方源码说明](third_party/README.md)和[目录与构建说明](docs/PROJECT_STRUCTURE.md)。当前产物为核心及组件静态库，不是可运行的看图程序。

## 设计方向

- C23 核心负责图片解码、数据预读、后台任务和缓存。
- C++23 + Dear ImGui 负责界面与窗口行为；必要时显式提供 C++20 兼容构建。
- 使用 CMake，计划接入 spdlog 日志与 Catch2 / CTest 测试。
- 计划支持中英双语；更新检测优先使用 cpp-httplib，但暂不实现，当前不引入其网络依赖。
- 计划支持 JPEG（JPG / JPEG）、PNG、SVG、WebP、BMP，并逐步扩展其他主流图片格式。
- 根据图片规模采用完整后显示、渐进更新或超大图分块处理。
- 保持公共 C API 独立，为后续 Win32 界面适配保留空间。
- 不计划支持 CAD、PSD 等专用设计源文件。

## 文档

- [首版设计文档](docs/DESIGN-v0.1.md)
- [工程落地与中英双语方案](docs/IMPLEMENTATION-v0.1.md)
- [Core API 设计](docs/CORE_API-v0.1.md) · [公共 C 头文件](core/include/liltview/core.h)
- [项目目录与 CMake 构建](docs/PROJECT_STRUCTURE.md)
- [第三方源码与 submodule 管理](third_party/README.md)
- [当前彩色图标说明](docs/BRANDING-v0.2.md)
- [命名与首版图标记录](docs/BRANDING-v0.1.md)

## 图标

<img src="assets/icons/png/liltview-color-256.png" alt="LiltView 彩色应用图标" width="128" height="128">

- [SVG 源图](assets/icons/liltview-color.svg)
- [Windows ICO](assets/icons/liltview-color.ico)
- [小尺寸 SVG](assets/icons/liltview-color-small.svg)
- [PNG 尺寸集合](assets/icons/png/)

当前选用彩色版本。窗口程序的运行及安装说明将在实现后补充。

## 许可证

Copyright 2026 LiltView contributors.

LiltView 采用 [Apache License 2.0](LICENSE)，SPDX 标识为 `Apache-2.0`。第三方依赖遵循各自的许可证。
