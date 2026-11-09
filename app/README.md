# 应用源码入口

模块边界、数据流和构建命令统一见[开发架构](../docs/ARCHITECTURE.md)，当前产品行为见[产品与设计](../docs/PRODUCT_DESIGN.md)。本文件只保留源码导航：

| 路径 | 职责 |
| --- | --- |
| `main.cpp`、`cli/` | CLI11 参数、启动配置与异常出口 |
| `application.*` | 窗口、事件循环、菜单同步、快捷键和退出编排 |
| `ui/`、`menu/` | 界面、窗口装饰、共享菜单模型 |
| `viewer/` | Core 事件消费、相机、动画、tile / LOD 场景缓存与后台恢复 |
| `renderer/` | 后端无关图片接口与 OpenGL 3.3 实现 |
| `platform/` | 字体、SDL render host、全局菜单、X11 / Wayland 窗口形状 |
| `settings/`、`i18n/`、`logging/` | YAML、安全替换、中英翻译、日志与 C 回调适配 |
| `resources/` | 内嵌彩色图标 |

显示、缓存与 PBO 细节见[渲染说明](../docs/RENDERING.md)，参数见[配置说明](../docs/CONFIGURATION.md)。首次 LOD / 压缩 / 写缓存后台化仍待实现，不能把后台恢复等同于整个首次加载都在后台。

窗口标题栏在原窗口 / GL context 上切换；X11 使用不透明 visual + Shape，Wayland 使用 alpha surface + input region。关闭统一先隐藏，再取消并等待后台工作、保存设置、撤销菜单关联、释放图像和 renderer，最后销毁窗口 / SDL。

`log_level: debug` 时的 `Close latency` 记录事件到处理、处理到退出请求及隐藏调用耗时，区分窗口 / 快捷键 / 窗口菜单 / D-Bus 来源。窗口菜单通过 ImGui EventId 关联；无法确定来源时标记 unavailable，不伪造零。计时使用 SDL 单调时间，不包括完整物理点击、合成器刷新或动画耗时。
