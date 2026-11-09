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

`viewer/` 管理原始分辨率流式场景、不可变分块缓存和动画；D3D11 renderer backend 后续实现。图片加载时经 C core event 上传；worker 线程不调用图形 API。静止时使用 SDL 事件等待。菜单退出、Ctrl+Q、原生窗口关闭和 SDL 退出事件共用关闭入口：收到请求后先隐藏窗口，不再处理后续交互、消费文件选择结果、pump 或提交最后一帧；随后正常取消并等待图片工作、保存设置、撤销系统菜单关联，最后释放 renderer image / backend / context，再关闭 ImGui / SDL。核心 join 使用退出条件变量的有界等待，而非等待 SDL 通知超时；没有强制终止线程或跳过资源回收。

关闭延迟诊断需要 `log_level: debug`。`Close latency` 日志分开记录：

- `event_to_handler_ms`：事件时间到进入 UI 线程处理的耗时。
- `handler_to_request_ms`：进入 UI 处理到调用 `request_quit()` 的耗时，包含窗口菜单等待 ImGui 消费输入 / 识别菜单项的时间。
- `event_to_request_ms`：上述两段之和；原有 `hide_ms` 单独统计请求隐藏的调用耗时。
- `source` 区分 `window_close`、`sdl_quit`、`shortcut`、`window_menu`、`global_menu`。
- `origin=sdl_event/sdl_key_down/sdl_mouse_release` 使用 SDL 事件提供的纳秒时间戳。窗口菜单按 ImGui 实际消费的 EventId 关联，不使用“最后一个鼠标事件”猜测；不能唯一关联时标记 `origin=unavailable`、`timing_valid=false`，缺失区间为 `-1`，而不是伪造零耗时。
- `origin=dbus_activation` 从应用进入 D-Bus 菜单激活回调开始计时，包含队列 / 主线程等待；不使用桌面面板传入的不同时间基准。

这些指标不等于物理点击到视觉消失：SDL 的关闭事件时间戳可能是在系统事件被 pump 时才生成，D-Bus 指标也不包含面板到应用的前段传输；隐藏之后的合成器画面更新 / 动画同样不在这些指标内。所有计算统一使用 SDL 的单调纳秒时间基准，出现缺失或逆序时间戳会明确标为不可用。

原生应用 ID 与 `.desktop` / icon 名保持一致，开发时用 `scripts/register_desktop.py` 注册。系统菜单适配对象在 `run()` 作用域内先销毁，撤销窗口关联并等待 D-Bus 工作线程退出，再由 Application 析构回收 GL、窗口与 SDL。

窗口装饰由 `ui/window_chrome` 的纯几何 / 命中测试和绘制实现，Application 将命中结果映射到 SDL hit test，让系统执行移动 / resize（包含 Wayland 的输入 serial），不通过鼠标事件循环模拟移动。标题栏 / 状态栏拖动与四边 / 四角 resize 分区，图片区域及按钮不拖窗；弹窗 / 文件对话框期间禁用自定义 hit test。主题边框及绘制尺寸共享同一套 DPI metrics。

默认 `system_titlebar=false`、`disable_rounding=false`；外观菜单提供系统标题栏及关闭圆角勾选。两项均参与菜单同步 / YAML 持久化。系统标题栏模式不注册自定义 hit test；X11 创建不透明 SDL 窗口且不应用 shape，Wayland 则为复用的 surface 显式设置全矩形不透明 / 输入区域；自绘圆角窗创建透明 OpenGL alpha framebuffer，背景、标题栏和主题边框按相同半径绘制；About 遮罩保持圆角外 alpha 为零。`platform/window_shape` 仅在窗口尺寸 / density / 半径变化时更新原生 shape：Wayland 通过私有队列绑定 compositor 一次并设置 input region；X11 设置 bounding / input Shape。Linux 区域按角部扫描线合并为 O(radius) 个矩形，而非每帧分配窗口大小 RGBA 掩码。最大化 / 全屏 / 系统标题栏时恢复矩形，Wayland proxy 在 SDL display 销毁前回收。

`update/` 延期，当前不创建网络模块或获取 cpp-httplib。

当前可打开 JPEG、PNG、基础 SVG、静态 WebP、BMP、GIF 自动播放和 TIFF 首页。CLI、菜单 / Ctrl+O 和拖放接入；目录导航仍禁用。

## 原始分辨率场景：解码一次，视口变换不重解码

静态图仅提交一个 `LV_TARGET_NATIVE` 请求，不先生成窗口大小的 FIT 缩略图，也不在平移 / 缩放时提交 REGION 请求。JPEG、非交错 PNG、未压缩 BMP 和 TIFF 逐行 / 行带解码：一个最终原始像素 tile 行带完成、通过源版本检查后就发布并显示，不等整幅图片 COMPLETE；尚未解码的区域保持空白。默认行带为 256 行，设备限制更小时缩小 tile，core 按队列预算背压并只保留活动行带。Adam7 PNG 仍需要合并缓冲，渐进 JPEG 仍需要系数工作集；这些格式不承诺立即产出首个行带，预算不足会明确失败。

小图的原始纹理和 mipmap 全部常驻 GPU，之后相机操作仅改变顶点 / UV，不重新解码、重采样或上传图片。100% 和整数倍放大对齐到物理像素，兼顾 HiDPI、奇数尺寸和 EXIF 方向。

超出 GPU 常驻预算的大图在首次加载时，把原始像素及一次性生成的各级纹理 LOD 无损压缩到匿名本地分块缓存；不是用一张全图缩略图代替高清内容。FIT 所需 LOD 放在少量共享纹理 atlas 中批量绘制，100% 只使用原始分辨率 tile；原始 tile 未进入显存时不拿低清层放大顶替。缓存读取 / 解压在独立后台线程执行，UI 仅提交限量 GPU 上传，按 LRU 淘汰非当前视口的高分辨率纹理；缩放和平移只选择已生成的纹理、更新坐标，必要时解压 / 重传本地缓存，不再读取原图解码，也不重新计算像素或 mipmap。FIT → 100%、往返拖动和不同窗口大小不会清空场景缓存。

缓存放在 `$XDG_CACHE_HOME/liltview`，未设置时使用 `$HOME/.cache/liltview`，以 0600 权限创建并立即 unlink，关闭、取消、失败或进程退出时回收；压缩后的单场景缓存上限为 16 GiB。core 的 384 MiB CPU / 4 MiB 事件队列预算和 renderer 的 GPU 图片预算分别执行，不承诺整个进程 RSS 硬上限。缓存 I/O / 显存不足显式报错，不静默退回缩略图。

### 平移 / 缩放交互调度

- `tile_reader` 只读取已有缓存 / 解压既有 LOD，不调用源图 codec、GL 或 ImGui。单场景解压 RAM LRU 上限 32 MiB、待处理队列最多 64 个请求、结果索引最多 512 项；旧 / 新场景可各有一个 reader，因此其 RAM 缓存同时存在时上限合计 64 MiB，另有单块有界工作缓冲。使用独立 fd 的 `pread`，不与 UI 的缓存追加共用 seek 位置；关闭时取消排队任务、等待正在执行的一块退出再关闭 fd。
- UI 按 tile 网格直接索引可见范围和一圈邻居，不在每帧扫描整个源图分块表；GPU LRU 只遍历当前驻留集合，旧视口保护只访问被保留的分块集合。
- 优先当前视口缺失项，连续操作替换过期排队请求；至多一个旧请求正在执行，它的不可变结果可以留在 RAM 缓存但不会提交过期相机。后台完成时唤醒 SDL，等待磁盘期间不靠持续自唤醒轮询。
- 正常上传按调用间约 2 ms / 4 MiB 限额提交；可见范围就绪后，空余预算允许时用约 1 ms、至多两块预热邻居，并保留最多 8 MiB / 总预算四分之一的准入余量。FIT 下只在 RAM 预读 native 中心视口及邻居，不预先改变 GPU 的 LOD 驻留，也不生成或上传全图原始显存副本。预热不淘汰正在使用的内容，失败不影响已完整显示的视口。
- OpenGL 的 `create_uploaded_image` 直接以完整像素创建 tile，跳过先清零后整块覆盖，并只生成一次 mipmap；旧 `create_image` 仍保留透明初始化，供渐进 atlas 等局部更新使用。上传同步消费指针，任意合法 stride / GL unpack 状态恢复契约不变。

以上限额不是单次驱动调用的硬实时保证；冷缓存大跳转仍可能短暂保留旧画面。首次加载时的 LOD 生成、压缩和缓存写入仍有 UI 工作，后续计划单独后台化，不将其写成已经解决。

`desktop.interaction` 删除原图后预热相邻分块，再检查连续小平移和往返同级缩放：不等待目标、零新增缓存读取 / 解压、零重复 GPU 上传，并输出包含离屏回读的 P50 / P95 样本。`tile_reader` 单测用阻塞 loader 验证 worker 卡住时 UI 的 request / find 仍能返回，另测最新任务替换、RAM LRU、fd / seek 所有权和显式错误重试。

### 缩放 / 平移的完整视口切换

首次打开仍允许原始行带逐步出现。相机变化采用独立的目标视口与已呈现视口，默认优先在 GPU 现有高清纹理可以完整覆盖的范围内推进相机，不再只因最终目标缺一块就冻结整个画面；目标就绪后准确到达最终相机。中间相机只访问足够分辨率的已驻留纹理，不拿低清 atlas 放大顶替，不露底色或半幅缺块。移动范围耗尽、远处冷缓存跳转时仍暂时保留最近完整视口，不能承诺任何跳转都即时。连续输入以最新目标为准，FIT / 缩放不会重新解码源图。

GPU 就绪按实际物理像素的覆盖检查，不把屏幕外 decode/filter halo 的相邻分块当成必须等待项；画布裁剪边界也对齐物理像素。中间相机使用仿射变换插值保持缩放锚点，每次推进都先检查完整高清覆盖，失败则不提交该相机。

恢复期间锁住最近已呈现视口正在使用的高分辨率纹理；同一 tile 的 LOD 升级也保留确切旧纹理，防止透明图在恢复中改变混合结果。上传成功才替换资源，取消或恢复失败不破坏旧画面；再次操作可重试。保留旧画面及准备目标同时计入既有 GPU 预算，预算不足明确报错而非强制淘汰旧画面或无限加显存。缓存未命中时会短暂保持上一视图，这是避免缺块 / 白块中间态的呈现取舍，并非缓存恢复零延迟。

`desktop.viewport_restore` 人为延迟上传以稳定产生多帧恢复，对浅色 / 深色背景、透明图逐帧比对旧画面，覆盖连续目标变更、取消、LOD 升级和上传失败；原图在恢复前已删除，保证不回读源图。该测试显式选择保留的 atomic 相机策略；应用默认使用 covered-motion 策略。`desktop.covered_motion` 用空间可识别的不透明 / 半透明图、上传延迟和已删除源文件验证：恢复未齐时平移 / 缩放确实推进，像素跟随相机，内容覆盖不露底色且透明度不重复混合；测试不等于冷区域零延迟保证。

GIF 播放继续复用两张有界 FIT 帧纹理，属于独立动画路径。WebP / RLE BMP / SVG 等暂不具备原始行带输出，仍按各自解码器的完整工作集和尺寸预算加载，但静态输出使用原始尺寸，不随相机操作重解码。

启用 `LILTVIEW_BUILD_DESKTOP_TESTS` 后，`desktop.viewer_sharpness` 校验原始逐像素棋盘格、透明镂空、非整数拖动和 1 / 1.25 / 1.5 / 2 倍 framebuffer 密度；删除原图后继续往返缩放、拖动并触发 GPU 缓存淘汰 / 恢复，验证不再依赖源图。core / viewer 测试另验证完成前行带可见、低于整图大小的内存预算、源版本、取消和资源释放。

## WSLg 缓存恢复上传与真实图片测量

恢复线程已经负责磁盘读取 / 解压，但它不能消除 UI 线程上的驱动上传阻塞。真实 12K–18K 图片复现后，OpenGL3 后端默认使用有界 PBO 暂存：最多 8 个槽、每槽最多 4 MiB；只在 fence 已完成时复用，不等待 fence。`Renderer::upload_ready()` 让可选的缓存恢复在槽满时留到下一帧，继续保留完整旧画面。通用上传仍可直接上传（例如大于暂存上限或槽满的非恢复调用），不改变同步借用像素接口；返回前已经复制调用方数据，GPU 只引用 renderer 自己的缓冲。

应用原始 tile 默认改为 512px（core 公共默认仍是 256px），同时服从设备上限，以减少中间缩放比例下的纹理数量和 draw calls。源图依旧只解码一次，首次按原始行带发布；不启用自定义 shader 或 RGB/YUV 新路径。

手动真实文件基准不加入 CTest，不把个人图片复制到仓库。启用 `LILTVIEW_BUILD_DESKTOP_TESTS` 后：

```sh
cmake --preset app-release -DBUILD_TESTING=ON -DLILTVIEW_BUILD_DESKTOP_TESTS=ON
cmake --build --preset app-release --target liltview liltview_viewer_benchmark -j 6
build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
# 同一构建、同一 tile 尺寸，关闭 PBO，仅作为直接上传对照：
LILTVIEW_BENCH_DIRECT=1 build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
```

基准输出实际 SDL video driver、GL vendor / renderer / version、实际 swap interval，以及初始全图、100%、平移 / 往返缩放的 pump / prepare / upload / submit / present、P50 / P95 / 最大帧、等待 / 停住的帧、读取 / 恢复次数和暂存资源计数。窗口隐藏、VSync 关闭，不含截图读回和应用菜单；平移帧之间 8ms、恢复帧之间 1ms 的测试节流只计入阶段总耗时，不计入单帧耗时。它用于隔离驱动和 viewer，不是完整用户窗口 FPS 或与其他应用的对比。冷区域仍需恢复，不能把帧耗时降低说成所有冷跳转零延迟。

装饰模式切换发生在 ImGui 帧外，先关闭非模态弹出菜单。Wayland 没有普通 toplevel 的全局位置控制，因此保留同一 SDL window / wl_surface、原地调整装饰及 opaque/input region，而不是创建新 surface 让合成器重新放置。X11 因不透明 / 透明 visual 不同仍创建隐藏窗口；先清空旧 front/back buffer（透明旧窗 alpha 清零），尽可能设置旧窗 opacity=0，再隐藏并同步。renderer 使用共享 GL context 保留纹理 / 字体 / cache 句柄，重新绑定 SDL ImGui backend 和原菜单服务；旧窗口销毁 / flush 后才显示新窗口。不能将原 GL context 直接绑定到不同 X11 visual 后继续 swap（软件 GL 可能在 X_ShmPutImage 才报 BadMatch）。

X11 用 XTranslateCoordinates 获取实际客户区坐标，不依赖可能过期的 SDL move 事件；按标题栏 / 边框高度差调整客户区位置与尺寸，保持图片视口锚点。首帧提交、窗口管理器完成边框配置后，再用 StaticGravity 按实际客户区坐标请求定位，不混用阴影 extents 和窗口边框偏移，并有界等待 WM 确认（最多 100ms，完成即返回），保留其他 WM hints，维持 StaticGravity，避免改回 gravity 时窗口再次被重新锚定，不通过固定 sleep 延迟掩盖问题；最终位置仍遵循窗口管理器的边界约束，允许少量系统边框偏差。Wayland 原地切换避免了二次映射的大幅位置变化。失效的旧窗口 resize/hide 事件不写回新窗口状态；文件选择对话框打开时拒绝切换，失败时恢复旧窗口可见性 / opacity 和原模式。
