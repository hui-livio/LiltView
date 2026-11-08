# 应用侧目录

启用 `LILTVIEW_BUILD_APP_COMPONENTS` 后创建 `liltview` 可执行文件、跨平台 renderer、ImGui UI 静态库、应用状态库和 spdlog 日志适配库。窗口 / 输入使用裁剪后的 SDL3 静态库；renderer 通过 CMake 选择 OpenGL 3.3 后端。渲染公共接口与 DX11 迁移边界见 [`docs/RENDERER-v0.1.md`](../docs/RENDERER-v0.1.md)。

逐步增加的职责分区：

- `logging/`：已建立核心日志回调到 spdlog 的异常隔离适配。
- `logging/configure.cpp`：启动读完 YAML 后将 log_level 应用到 spdlog；Debug 默认 debug，发布构建默认 warn，有效配置优先。
- `main.cpp`：解析结果与应用启动编排、异常出口。`cli/options.cpp` 用 CLI11 维护参数、帮助、版本和输入校验，返回前不初始化 SDL。
- `application.cpp`：`run()` 仅编排初始化、事件循环及设置保存；窗口、ImGui、字体、事件、快捷键、菜单同步、渲染和冒烟测试分成独立函数，注释说明线程与释放顺序。
- `menu/`：共用菜单树及稳定动作 ID，供系统菜单和窗口内回退菜单使用。
- `ui/`：`Shell::draw()` 只编排窗口菜单、工具栏、画布、状态栏和 About，各区域独立函数。About 分为应用信息、许可证、作者与链接，包含 hui-livio、邮箱和项目地址。
- `i18n/`：中英字符串、回退与运行时语言选择；控件使用稳定 ID。
- `settings/`：iguana 读写 settings.yaml，反射结构与字段校验独立于运行时 enum。不含 INI 兼容 / 迁移逻辑。`store.cpp` 检查写入、刷新和关闭后替换，保留权限与有效符号链接；失败清理临时文件。详见[配置说明](../docs/CONFIGURATION.md)。
- `platform/`：字体候选查找与加载；读取前限制大小，校验随包字体 CRC32，解析失败尝试系统字体。Linux 用 GIO 导出 `com.canonical.dbusmenu`，包含全项批量查询和属性默认值；Wayland / X11 分别关联窗口，后台线程只排队动作，通过 SDL 唤醒主线程。

可读性审查记录见 [函数拆分与验证](../docs/CODE_REVIEW-2026-11-07.md)。D-Bus 方法解析、服务生命周期、Wayland / X11 关联分别有独立入口；字体查找分为随包资源与平台回退。
- `renderer/`：跨平台 image/frame API、句柄校验与 OpenGL 后端；图形对象不出现在 app/controller 接口。
- `resources/`：从 256px 项目图标生成的 RGBA 数据，无运行时图标解码依赖；通过 renderer 上传后使用 mipmap 缩小采样。

后续增加 `viewer/` 和 D3D11 renderer backend。图片加载时经 C core event 上传；worker 线程不调用图形 API。静止时使用 SDL 事件等待，退出时先释放 renderer image / backend / context，再关闭 ImGui / SDL。

原生应用 ID 与 `.desktop` / icon 名保持一致，开发时用 `scripts/register_desktop.py` 注册。系统菜单适配对象在 `run()` 作用域内先销毁，撤销窗口关联并等待 D-Bus 工作线程退出，再由 Application 析构回收 GL、窗口与 SDL。

`update/` 延期，当前不创建网络模块或获取 cpp-httplib。

当前可打开 JPEG、PNG、基础 SVG、静态 WebP 和 BMP，viewer 每帧最多消费约 4 ms 的 core 事件，上传返回即 release 像素。CLI、菜单 / Ctrl+O 和拖放接入；目录导航仍禁用。FIT 输出受 framebuffer 和设备上限约束，100% 仅在 FIT 未缩小源图时可用。
