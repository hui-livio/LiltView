# 应用侧目录

启用 `LILTVIEW_BUILD_APP_COMPONENTS` 后创建 ImGui 依赖目标、UI 接口目标和 spdlog 日志适配库。当前没有窗口可执行文件，窗口 / 渲染后端待选。

逐步增加的职责分区：

- `logging/`：已建立核心日志回调到 spdlog 的异常隔离适配。
- `ui/`：ImGui 菜单、面板、控件与交互。
- `viewer/`：目录导航、有效请求集合、缩放和平移。
- `renderer/`：像素上传、纹理与 GPU 预算。
- `platform/`：窗口、输入、文件选择和平台事件唤醒。
- `i18n/`：中英字符串、回退与运行时语言选择。

`update/` 延期，当前不创建网络模块或获取 cpp-httplib。
