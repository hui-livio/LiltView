# LiltView

轻量、流畅的图片浏览器，首发目标平台为 Arch Linux。

当前处于设计阶段，仓库包含设计文档、公共 C API 头文件草案和图标资源，尚未实现可运行程序。

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
- [当前彩色图标说明](docs/BRANDING-v0.2.md)
- [命名与首版图标记录](docs/BRANDING-v0.1.md)

## 图标

<img src="assets/icons/png/liltview-color-256.png" alt="LiltView 彩色应用图标" width="128" height="128">

- [SVG 源图](assets/icons/liltview-color.svg)
- [Windows ICO](assets/icons/liltview-color.ico)
- [小尺寸 SVG](assets/icons/liltview-color-small.svg)
- [PNG 尺寸集合](assets/icons/png/)

当前选用彩色版本。构建、运行及安装说明将在实现阶段补充。
