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

`viewer/` 管理原始分辨率流式场景、不可变分块缓存和动画；D3D11 renderer backend 后续实现。图片加载时经 C core event 上传；worker 线程不调用图形 API。静止时使用 SDL 事件等待，退出时先释放 renderer image / backend / context，再关闭 ImGui / SDL。

原生应用 ID 与 `.desktop` / icon 名保持一致，开发时用 `scripts/register_desktop.py` 注册。系统菜单适配对象在 `run()` 作用域内先销毁，撤销窗口关联并等待 D-Bus 工作线程退出，再由 Application 析构回收 GL、窗口与 SDL。

`update/` 延期，当前不创建网络模块或获取 cpp-httplib。

当前可打开 JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放和 TIFF 首页。CLI、菜单 / Ctrl+O 和拖放接入；目录导航仍禁用。

## 原始分辨率场景：解码一次，视口变换不重解码

静态图仅提交一个 `LV_TARGET_NATIVE` 请求，不先生成窗口大小的 FIT 缩略图，也不在平移 / 缩放时提交 REGION 请求。JPEG、非交错 PNG、未压缩 BMP 和 TIFF 逐行 / 行带解码：一个最终原始像素 tile 行带完成、通过源版本检查后就发布并显示，不等整幅图片 COMPLETE；尚未解码的区域保持空白。默认行带为 256 行，设备限制更小时缩小 tile，core 按队列预算背压并只保留活动行带。Adam7 PNG 仍需要合并缓冲，渐进 JPEG 仍需要系数工作集；这些格式不承诺立即产出首个行带，预算不足会明确失败。

小图的原始纹理和 mipmap 全部常驻 GPU，之后相机操作仅改变顶点 / UV，不重新解码、重采样或上传图片。100% 和整数倍放大对齐到物理像素，兼顾 HiDPI、奇数尺寸和 EXIF 方向。

超出 GPU 常驻预算的大图在首次加载时，把原始像素及一次性生成的各级纹理 LOD 无损压缩到匿名本地分块缓存；不是用一张全图缩略图代替高清内容。FIT 所需 LOD 放在少量共享纹理 atlas 中批量绘制，100% 只使用原始分辨率 tile；原始 tile 未进入显存时不拿低清层放大顶替。缓存恢复每帧约 4 ms、按 LRU 淘汰非当前视口的高分辨率纹理；缩放和平移只选择已生成的纹理、更新坐标，必要时解压 / 重传本地缓存，不再读取原图解码，也不重新计算像素或 mipmap。FIT → 100%、往返拖动和不同窗口大小不会清空场景缓存。

缓存放在 `$XDG_CACHE_HOME/liltview`，未设置时使用 `$HOME/.cache/liltview`，以 0600 权限创建并立即 unlink，关闭、取消、失败或进程退出时回收；压缩后的单场景缓存上限为 16 GiB。core 的 384 MiB CPU / 4 MiB 事件队列预算和 renderer 的 GPU 图片预算分别执行，不承诺整个进程 RSS 硬上限。缓存 I/O / 显存不足显式报错，不静默退回缩略图。

GIF 播放继续复用两张有界 FIT 帧纹理，属于独立动画路径。WebP / RLE BMP / SVG 等暂不具备原始行带输出，仍按各自解码器的完整工作集和尺寸预算加载，但静态输出使用原始尺寸，不随相机操作重解码。

启用 `LILTVIEW_BUILD_DESKTOP_TESTS` 后，`desktop.viewer_sharpness` 校验原始逐像素棋盘格、透明镂空、非整数拖动和 1 / 1.25 / 1.5 / 2 倍 framebuffer 密度；删除原图后继续往返缩放、拖动并触发 GPU 缓存淘汰 / 恢复，验证不再依赖源图。core / viewer 测试另验证完成前行带可见、低于整图大小的内存预算、源版本、取消和资源释放。
