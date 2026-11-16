# 应用源码入口

模块边界、数据流和构建命令统一见[开发架构](../docs/开发架构.md)，当前产品行为见[产品与设计](../docs/产品与设计.md)。本文件只保留源码导航：

| 路径 | 职责 |
| --- | --- |
| `main.cpp`、`cli/` | CLI11 参数、启动配置与异常出口 |
| `application.*` | 窗口、事件循环、菜单同步、快捷键和退出编排 |
| `ui/`、`menu/` | 界面、窗口装饰、共享菜单模型 |
| `viewer/` | Core 事件消费、相机、动画、tile / LOD 场景缓存与后台恢复 |
| `renderer/` | 后端无关图片接口与唯一 SDL_Render 实现 |
| `platform/` | 字体、SDL render host、Linux 全局菜单、X11 / Wayland 窗口形状，以及 Win32 文件 I/O、窗口主题和命中测试 |
| `settings/`、`i18n/`、`logging/` | YAML、安全替换、中英翻译、日志与 C 回调适配 |
| `resources/` | 内嵌彩色图标、Windows RC 图标 / 版本信息模板 |

显示、缓存与 SDL 纹理上传细节见[渲染说明](../docs/渲染与大图实现.md)，参数见[配置说明](../docs/命令行与设置.md)。首次加载通过有界 TilePreparation worker 进行像素打包、LOD、压缩和缓存写入；SDL 纹理创建与上传仍由主线程执行，首屏完整后统一显示。

窗口标题栏在原窗口 / SDL_Renderer 上切换；Linux X11 使用不透明 visual + Shape，Wayland 使用 alpha surface + input region。应用仅使用 SDL_Render 与 ImGui SDLRenderer3，不创建图形 API 上下文、不直接链接 OpenGL，也不维护独立 D3D 后端；底层驱动由 SDL 选择。关闭统一先隐藏，再取消并等待后台工作、保存设置、撤销菜单关联、释放图像和 renderer，最后销毁窗口 / SDL。

`log_level: debug` 时的 `Close latency` 记录事件到处理、处理到退出请求及隐藏调用耗时，区分窗口 / 快捷键 / 窗口菜单 / D-Bus 来源。窗口菜单通过 ImGui EventId 关联；无法确定来源时标记 unavailable，不伪造零。计时使用 SDL 单调时间，不包括完整物理点击、合成器刷新或动画耗时。
