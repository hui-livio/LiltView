# 渲染与大图实现

文档核对：2026-11-11，按当前源码声明实现边界。模块全景见[架构](ARCHITECTURE.md)，接口声明以 [`renderer.hpp`](../app/renderer/renderer.hpp) 为准；未完成工作集中在[开发计划](DEVELOPMENT_PLAN.md)。

## 1. 当前显示链路

```text
静态图：单次 DISPLAY / NATIVE
  → core worker 解码 → IMAGE_INFO → 原始像素行带 / 完整输出 → COMPLETE
  → Viewer / Tiles → GPU 常驻，或建立本地 tile / LOD 缓存
  → 相机选择已生成纹理；缺失时 TileReader 读取 / 解压 → 限量 GPU 上传
  → ImGui draw list → OpenGL 3.3 → present
GIF：动画会话 → 完整 FIT 帧 → 候选纹理 → deadline 到达后整帧交换
```

- 静态 viewer 不先生成 FIT 缩略图，也不因平移、缩放或窗口变化重新提交 REGION。core 的 FIT / REGION 仍是独立可调用能力。
- JPEG、非交错 PNG、未压缩 BMP、TIFF 可在每个原始行带完整、版本检查通过后发布，先完成的行带先显示；未解码区域仍为空白。
- 应用 tile 默认边长为 512px，按设备上限缩小；core 公共默认是 256px。事件队列背压使 core 只保留有界活动行带，不要求整幅 RGBA 常驻。
- SVG 仅对[基础静态 profile](IMAGE_FORMATS.md#4-svg-静态-profile)栅格化，复杂 SVG 不支持。当前与其他完整输出静态图一样，按自然尺寸生成像素后进入 tile / LOD 路径；放大不重新执行矢量渲染，没有独立 SVG 文档渲染通道，也不保证任意放大保持矢量清晰。
- Progressive JPEG 仍需系数工作集，Adam7 PNG 仍需合并缓冲；静态 WebP、SVG、RLE BMP 等走各自完整工作集路径，不承诺首行带时延。
- 加载完成后，相机操作只选择已有像素 / LOD、改变坐标和 UV。GPU 已驻留内容不重复上传；缺失内容从场景缓存恢复，不重新读取源文件解码。

## 2. 清晰度、方向与完整画面

- 相机和源坐标使用 double，减去相机位置后转为 GPU float；viewer 统一应用方向变换。100% 及整数倍放大对齐物理像素，考虑 HiDPI 和奇数尺寸。
- 原始细节放大使用 nearest，缩小使用既有 LOD / 过滤；FIT 是同一原始场景的显示方式。超出单纹理尺寸的源图使用多个局部纹理，不分配源图大小的单张 GPU texture。
- 大图 FIT 使用共享 atlas 批量绘制，100% 只采用原始分辨率 tile。低清层不放大替代尚未恢复的原始细节。
- 首次加载允许行带逐步出现；加载后的交互维护目标相机与已呈现相机。默认在现有高清纹理完整覆盖范围内推进，保持缩放锚点，目标就绪后准确到达。
- 冷区域超出现有覆盖时暂时保留最近完整画面。恢复期间保护旧视口纹理，包括同 tile 的旧 LOD；上传成功才替换，避免空白块、透明度重复混合和部分切换。
- 旧画面与候选资源同时计入 GPU 图片预算；失败保留可用旧图并报告错误。测试还保留整视口就绪后切换的策略作对照，应用默认并非此策略。
- 16K / 32K 是现有回归覆盖点，不是统一源尺寸上限；实际限制来自格式、工作集、缓存和设备。强缩小时 atlas 的混叠质量仍待完善。

## 3. 场景缓存与后台恢复

| 资源 | 当前实现 / 上限 |
| --- | --- |
| 小图 | 原始 tile 与 mipmap 驻留 GPU |
| 大图本地缓存 | 首次生成原始 tile 与各级 LOD，无损压缩；单场景压缩数据最多 16 GiB |
| 缓存文件 | `$XDG_CACHE_HOME/liltview`，否则 `$HOME/.cache/liltview`，均缺失时为系统临时目录下的 `liltview`；0600 创建后立即 unlink，通过 fd 使用，场景销毁时释放 |
| TileReader | 每场景独立线程与 fd，通过 `pread` 读取；只读 / 解压已有数据，不调用 codec、GL 或 ImGui |
| RAM LRU | 每 reader 32 MiB；旧 / 新场景并存时最多 64 MiB，另有有界单块工作缓冲 |
| 请求与索引 | 待处理最多 64 项，结果索引最多 512 项；连续输入替换过期排队任务，当前执行块可结束并缓存 |
| 上传调度 | 正常恢复按调用间约 2 ms / 4 MiB 限额；空余资源下约 1 ms、最多两块邻居预热 |

可见 tile 由网格直接索引，GPU LRU 只遍历驻留集合；后台完成后唤醒 SDL，不靠持续轮询。预热优先不淘汰正在使用的内容；FIT 可在 RAM 预读原尺寸中心及邻居，不预先上传全图原始纹理。

该缓存服务当前场景，关闭后不保留；它与 Core 可选跨请求内存 LRU 分开计账，也不是跨启动缓存。应用启用 64 MiB 内容缓存子预算并接入静态邻居 PREFETCH。静态预读与 GIF 预热合计最多两个未终结后台 ID（含已取消旧任务），不上传 GPU，不改变当前错误或信息。当前显示 / 恢复优先；被抢占的同计划静态预读只在新的忙闲边沿最多重试两次，不自唤醒。首次 LOD 生成、压缩和追加写缓存仍在 UI 路径，尚未全部后台化。

## 4. Renderer 接口与所有权

| 接口 / 对象 | 必要契约 |
| --- | --- |
| `Image` | 不透明 owner / slot / generation，拒绝失效或其他 renderer 的句柄；销毁失效句柄幂等 |
| `Pixels` | 左上原点、RGBA8、straight alpha；使用真实 stride / byte_count，不做颜色转换 |
| `create_image` | 分配并透明初始化，允许随后局部更新；尚未填充区域不能含未定义显存 |
| `create_uploaded_image` | 事务式创建完整图，失败回收候选；可跳过透明清零并只生成一次 mipmap |
| `update_image` | 受检目标矩形、尺寸、跨度及 buffer；返回或抛异常后均不保留调用方 CPU 指针 |
| `add_image` / `add_image_quad` | 只向 draw list 追加命令；EXIF 通过顶点 / UV 表达，不在这里解码或旋转 CPU 像素 |
| `capabilities` / `upload_stats` | 分别报告图片预算与 PBO 暂存计数；不是系统真实显存硬上限 |

所有图形调用都在渲染线程。core event / pixels 在同步上传返回后即可 release；延后读取 core 数据则先 retain。直接路径按原 stride 使用 `GL_UNPACK_ROW_LENGTH`，不能表达的跨度逐行上传；保存并恢复 pixel-store、unpack PBO 和纹理绑定等状态。

core 最终输出到事件交接使用引用转移 / 共享 backing，不深拷贝整幅像素。但缓存编码、LOD 生成及 PBO 有明确工作缓冲，GPU 上传也有主存到设备传输，不宣称全链路零拷贝。

帧顺序：`begin_frame → platform NewFrame → ImGui NewFrame → 布局 / draw list → ImGui Render → render_frame → 可选 readback → present`。图片最后一次绘制提交后才销毁；画布箭头动作发生在 draw list 构建之后，切图时将 pending scene 转入 retired scene，保留到本帧提交，下一次 `pump()` 再清理，连续切图不能提前销毁被引用纹理。退出显式清理 retired scene；viewer 图片先于 renderer、ImGui context 和原生窗口释放。

## 5. 有界 PBO 上传

- OpenGL 默认启用 8 个槽、每槽至多 4 MiB 的 PBO / fence；逻辑暂存上限 32 MiB，独立于默认 128 MiB 图片预算统计。
- 仅当 fence 已完成才复用，零超时检查；缓存恢复通过 `upload_ready()` 延到后续帧，不等待 GPU fence。
- 返回前调用方字节已复制到 renderer 自有缓冲，GPU 后续不读取调用方指针；公共同步借用契约保持。
- 超大跨度、诊断模式或普通上传槽满可走 direct fallback。因此 2 ms 调度不是单次驱动调用的硬实时保证，也没有新增后台 GL context。
- 图片仍通过 ImGui OpenGL3 backend 绘制 RGBA8；独立图像 shader、RGB / YUV 原生纹理和 GPU 颜色转换尚未实现。

## 6. 动画与平台边界

GIF 使用当前显示和候选两张有界 FIT 纹理。候选完整、COMPLETE 已到且播放 deadline 满足后交换；动画不做半帧揭示。两张纹理都计预算，不足时保留当前完整帧并停止播放、报告错误。时间、循环、epoch 与引用规则见 [Core 契约](CORE_CONTRACTS.md)。

GIF 邻居通过内容识别后预解码 frame 0，保留 CPU 像素与会话，不创建 GPU 资源。切入 READY 时先 NEXT 重验版本，取得真实 frame 1 或 END；源变化、FIT 边界变化或上传失败回退普通打开。两帧局部纹理全部上传成功才提交 frame 0 并开始时钟，该次 pump 不切到后继，避免慢上传吞掉首帧。预热 retained reservation 为 64 MiB，含 backing 池超配与描述符，不等于 RSS；实际像素仍计 Core CPU，采用后的当前 / 候选纹理计 renderer 图片预算。它不是完整动画内容缓存。

Linux 使用 SDL3 / OpenGL 3.3。D3D11 / Win32 后端未实现；未来须保持同一句柄、stride、同步借用、采样、失败回收和线程契约，另验收 resize、DPI、device removal 与设备资源释放。

## 7. 复现与验收

功能回归入口：[`viewer_sharpness.cpp`](../tests/integration/viewer_sharpness.cpp)、[`renderer_contract.cpp`](../tests/integration/renderer_contract.cpp)、[`large_render.cpp`](../tests/integration/large_render.cpp)、[`tile_reader_tests.cpp`](../tests/app/tile_reader_tests.cpp)。覆盖原图删除后继续缩放 / 恢复、热交互零额外读取 / 上传、透明图完整覆盖、EXIF、HiDPI、不规则 stride、PBO / direct 状态恢复与释放。测试存在不等于本次重新执行。

手动真实图片基准不加入 CTest，也不要求把个人文件提交到仓库：

```sh
cmake --preset app-release -DBUILD_TESTING=ON -DLILTVIEW_BUILD_DESKTOP_TESTS=ON
cmake --build --preset app-release --target liltview liltview_viewer_benchmark
build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
LILTVIEW_BENCH_DIRECT=1 build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
```

基准记录实际 SDL / GL 驱动、swap interval、pump / prepare / upload / submit / present、帧分位数、目标完成延迟、读取 / 恢复和资源计数。隐藏窗口、关闭 VSync，不含截图读回或应用菜单；帧间测试节流计入阶段总时间，不计单帧。完整矩阵与采集口径见[性能测试计划](../benchmarks/TEST_PLAN.md)。

保留的历史实测（2026-11-09，非本次重测）：Arch / WSLg、SDL x11、D3D12 / RTX 3060、Mesa 26.2.6、Release、1600 × 1000、swap interval=0、图片预算 128 MiB。下表为同一 512px 构建的单次诊断对照，输入 `moban.png` 为 12144 × 10384 RGB8 非交错 PNG：

| 指标 | direct | PBO |
| --- | ---: | ---: |
| 100% 冷纹理恢复 | 706.4 ms | 89.3 ms |
| 首次平移帧 P95 / 最大帧 | 10.00 / 60.54 ms | 6.26 / 9.36 ms |
| 第 8 步缩小完成 | 808.8 ms | 280.3 ms |
| 下一冷层级完成 | 1485.9 ms | 696.5 ms |

原 256px direct 版本同图为 642.3 ms / 平移 P95 17.71 ms；不能把 tile 变更收益全部归给 PBO。原记录另测 17952 × 13200 PNG、12122 × 10866 JPEG，PBO 冷 100% 为 87.1 / 69.1 ms；三图重复平移均无新增读取 / GPU 恢复。未控制 OS 页缓存，未做重复实验置信区间；这些不是完整窗口 FPS、固定延迟或与 VS Code 的对比结论。

历史用户反馈仍为“有所优化，卡顿未解决”。冷 LOD、首次缓存生成和全图加载仍待优化；初始全图完成曾约 17.9 / 155.8 / 13.7 s，尚未拆清源 I/O、解码和缓存各自成本。

## 8. 窗口边框与圆角

- 自绘外轮廓使用边框色填充，内部背景缩进 1 DIP；直线段补在前景，不再对圆角叠加描边，避免圆弧变粗和浅色背景漏入透明边缘。A 方案边框色：浅色 `#818994`、深色 `#6F8297`。
- 该方案强调柔和边界，不沿用旧边框的 4.5:1 测试门槛：浅色普通背景至少 3:1，较暗菜单条至少 2.5:1，深色背景至少 4:1；测试同时固定 RGB，避免仅凭对比度放过错误配色。这不是文字对比度或整套界面的无障碍合规声明。
- 圆弧绘制期间局部提高 ImGui 的曲线采样精度，并依据 framebuffer 密度缩小允许误差；绘制结束恢复共享参数。覆盖四角、100% / 125% / 150% / 200% UI 缩放与 framebuffer 密度的几何及像素测试。
- Windows 透明窗口的 HRGN 是二值视觉裁剪，不能直接复用按像素中心生成的圆角输入区域。视觉区域为圆弧外的抗锯齿带保留 1 个逻辑像素余量，最终覆盖率由 framebuffer alpha 与 DWM 合成，避免半透明像素被硬裁剪成台阶；不改变原有 6 DIP 缩放命中区和原生命中回调。
- 系统标题栏、直角自绘、最大化时仍移除 HRGN；保留自绘非客户区保护及原生 sizing timer 重绘路径，不恢复经典标题栏，不改变拖窗/拉伸机制。GPU 回归同时检查四角半透明像素是否位于真实 HRGN 内，而不只检查 framebuffer。
- Wayland 保留独立输入区域与透明合成；X11 当前仍使用不透明 visual 和二值 ShapeBounding，曲线绘制精度有所提高，但无法承诺其外轮廓具备透明合成级别的平滑效果。没有为此引入框架或更改 X11 visual。
