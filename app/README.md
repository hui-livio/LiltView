# 应用侧目录

启用 `LILTVIEW_BUILD_APP_COMPONENTS` 后创建 `liltview` 可执行文件、ImGui UI 静态库、应用状态库和 spdlog 日志适配库。窗口使用 submodule 中裁剪后的 SDL3 静态库，渲染使用系统 OpenGL 3.3 和 ImGui 官方匹配后端。

逐步增加的职责分区：

- `logging/`：已建立核心日志回调到 spdlog 的异常隔离适配。
- `main.cpp`：命令行与异常出口。
- `application.cpp`：`run()` 仅编排初始化、事件循环及设置保存；窗口、ImGui、字体、事件、快捷键、菜单同步、渲染和冒烟测试分成独立函数，注释说明线程与释放顺序。
- `menu/`：共用菜单树及稳定动作 ID，供系统菜单和窗口内回退菜单使用。
- `ui/`：`Shell::draw()` 只编排窗口菜单、工具栏、画布、状态栏和 About，各区域独立函数。About 分为应用信息、许可证、作者与链接，包含 hui-livio、邮箱和项目地址。
- `i18n/`：中英字符串、回退与运行时语言选择；控件使用稳定 ID。
- `settings/`：窗口尺寸、最大化、三态主题和语言的解析与序列化。`store.cpp` 检查写入、刷新和关闭，再替换配置，保留权限与有效符号链接；失败清理临时文件并记录错误。主题默认跟随系统，手动深浅选择保持优先。
- `platform/`：字体候选查找与加载；读取前限制大小，校验随包字体 CRC32，解析失败尝试系统字体。Linux 用 GIO 导出 `com.canonical.dbusmenu`，包含全项批量查询和属性默认值；Wayland / X11 分别关联窗口，后台线程只排队动作，通过 SDL 唤醒主线程。

可读性审查记录见 [函数拆分与验证](../docs/CODE_REVIEW-2026-11-07.md)。D-Bus 方法解析、服务生命周期、Wayland / X11 关联分别有独立入口；字体查找分为随包资源与平台回退。
- `resources/`：从 256px 项目图标生成的 RGBA 数据，无运行时图标解码依赖；使用 mipmap 缩小采样并对齐实际像素。

后续增加 `viewer/` 和图片 `renderer/`。当前不提交图片加载请求，不调用尚未实现的核心引擎入口。静止时使用 SDL 事件等待，退出时先释放 ImGui / GL 资源再关闭 SDL。

原生应用 ID 与 `.desktop` / icon 名保持一致，开发时用 `scripts/register_desktop.py` 注册。系统菜单适配对象在 `run()` 作用域内先销毁，撤销窗口关联并等待 D-Bus 工作线程退出，再由 Application 析构回收 GL、窗口与 SDL。

`update/` 延期，当前不创建网络模块或获取 cpp-httplib。
