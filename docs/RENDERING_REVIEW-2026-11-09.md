# 当前原始分辨率加载 / 缓存 / OpenGL 路径 review

日期：2026-11-09。范围：当前工作区相对 HEAD 的 native 行带加载、viewer / Tiles、本地缓存和 OpenGL 上传改动。**本次不实现自定义 shader，也不把 RGBA8 输出接口改为 RGB / YUV。** 后续工作见 [开发计划](RENDERING_PLAN.md)。

## 结论与当前事实

- 静态图改为单次 `LV_TARGET_NATIVE`；相机方法不提交 NATIVE / REGION 请求。JPEG / PNG / BMP / TIFF 的原始最终行带可以先发布、先显示；算法自身的 progressive / interlace 工作集限制仍保留。
- 小场景驻留 GPU；大场景使用无损压缩原始 tile / 预生成 LOD 缓存、FIT atlas 和有界 GPU 高分辨率 LRU。GPU 缓存恢复不是源图重解码，也不应宣传为完全没有 I/O / CPU 工作。
- 当前公共像素输出和 renderer `Pixels` **仍是 RGBA8 单平面**，OpenGL backend 使用 `GL_RGBA8` / `GL_RGBA`；图片经过 ImGui OpenGL3 现有 shader，并非当前已有专用 RGB / YUV 图像 pass。
- 不能据此认定剩余耗时都在 shader：core 解码、UI 线程的 LOD / 压缩 / 缓存 I/O 和 GL 上传必须分别测量。当前功能测试不是与 VS Code 或其他实现的性能对比。

## 已发现并修复

### [P1] 非均匀 tile 的 FIT atlas 首次写入遗漏，且有纹理泄漏

位置：`app/viewer/tiles.cpp`，`Tiles::upload()` / `upload_overview()` / `clear()`。

首次遇到某类像素时的 cache-miss 分支仍创建独立 overview 纹理；纯色缓存命中分支才写共享 atlas。但 FIT 已改为只绘制 atlas，cached 场景清理也只释放 atlas。结果是非均匀图像可能 FIT 空白，首次独立纹理残留；纯色 checkerboard 的后续命中会掩盖问题。

修复：所有 cached tile 的 overview 写入统一经过 `upload_overview()`，共享纹理由 atlas 容器统一释放。回归对逐像素非均匀棋盘和透明镂空检查 FIT 实际像素，关闭 scene 后检查 renderer 图片账本回到零。

### [P1] 换图时旧场景高分辨率驻留阻塞新场景显存准入

位置：`app/viewer/viewer.cpp` 的静态 INFO 处理；`Tiles::release_details()`。

`make_room()` 只管理当前 scene 的 LRU；旧 scene 可以占满显存，新候选即使可通过回收旧高分辨率纹理装入，也会报预算错误。

修复：收到新静态 INFO 时释放旧 scene 的可恢复高分辨率 GPU 驻留，保留旧原始缓存 / FIT atlas 和源锚点。新图失败仍可回到旧图，不为了准入删除唯一原始结果。回归在大图拖动填充缓存后替换为另一幅 native 图，并检查像素和资源释放。

### [P1] TIFF 原生快速拷贝误将未声明的额外通道当作 alpha

位置：`core/src/codecs/tiff.c`，原尺寸 little-endian 拷贝分支。

`!alpha` 不等于文件没有 extra samples。未声明语义的额外通道必须按既有契约忽略、输出不透明；直接复制 libtiff 的 alpha 字节可能输出 0，使原图意外透明。

修复：只有 `extras == 0` 才进入不透明快速拷贝，其他文件沿用显式 alpha 解释 / 不透明规范化。回归同时覆盖 FIT 和 NATIVE 下 tiled / stripped、planar / contiguous、8 / 16-bit、关联 / 非关联 / 未声明 extra samples。

### [P2] 完全驻留的小图上传前存在不必要的像素打包复制

位置：`Tiles::upload()` / `upload_texture()`。

原实现先复制最终 `lv_pixels` 到紧密数组，再上传，即使没有缓存编码需求也增加一次拷贝，并破坏已有传输所有权约束。

修复：resident 路径直接借用 core 的 data / stride / byte_count，同步上传后即可 release。只有显式生成大图压缩缓存 / LOD 时使用有界工作缓冲；这是缓存编码工作，不再声称所有缓存生成都零拷贝。

## 仍需后续处理的 review 项

### [P2，待开发] UI 时间片不约束单次同步工作

位置：`Viewer::pump()`；`Tiles::upload()` / `prepare()`。

4 ms deadline 只在调用之间检查。一次 LOD 生成、zlib 编码、文件 seek / read / write 或 GL 更新超过 4 ms 时仍会阻塞 UI；目前没有每阶段的 P95 / P99 数据。按开发计划 P0 / P1 先测量，再迁移有界后台缓存任务；不可仅更换 shader 就宣称解决。

### [P2，待开发] 强缩小时 FIT atlas 的 minification 质量

位置：`upload_overview()` 和 `draw()`。

FIT atlas 当前没有完整 GPU mip 链，使用线性 minification；初始 LOD 由预算选取，窗口大幅缩小后可能出现混叠。原始 LOD 虽已在缓存中生成，但尚未完成动态选择 / atlas 层级驻留策略。后续从既有 LOD 恢复，或在批次完成时生成 GPU mip 链；不得每次相机变换重新生成像素。100% 的原始像素校验不等于所有 FIT 缩小质量均已验收。

### [P2，待开发] app 自身缓存账本需补齐

位置：`Tiles::begin()` / `open_cache()` / `uniform_levels_` / `levels`。

core CPU 预算和 renderer 图片预算不包括全部 app 场景索引、LOD 描述符、压缩暂存 / zlib 和文件系统缓存。当前索引有准入上限、每场景压缩文件上限 16 GiB，但旧 / 新场景同时存在时并非进程级总额度。后续统一旧 / 新场景和缓存 / 暂存账本，补磁盘故障注入；不得将 core 预算写成整个进程 RSS 硬限制。

## 测试与证据

- core：NATIVE 原尺寸、输出行带顺序、partial edge tiles、四类区域 codec、32 MiB 原图在 12 MiB core 预算下完成，不分配完整原始 RGBA 输出。
- viewer：完成前原始行带可见；加载后删除 / 替换源文件，缩放 / 平移不重新读取源图；小图常驻时上传 / 创建次数不变。
- OpenGL：非均匀 FIT atlas、逐像素原始细节、透明镂空、非整数拖动、HiDPI，删除原图后的 LRU 淘汰 / 恢复，以及新源替换和图片资源回收。
- 大图：16K PNG / JPEG / BMP、32K TIFF 及 128px 单纹理限制下 FIT / native / pan / zoom。完整回归执行命令：`ctest --test-dir build/dev --output-on-failure -j 6`。
- 本轮完整回归：**118 / 118 通过**，包含 16K / 32K 大图及 offscreen OpenGL；`git diff --check` 通过。
- 测试结果以本轮命令输出为准；Debug / offscreen / 合成图功能测试不代表 Release 下用户真实图片的响应时延，也没有证明自定义 shader 的收益。

## 本轮明确未做

未新增 GLSL、RGB8 / NV12 / I420 输入、plane 描述结构、YUV→RGB GPU 转换、PBO 或异步上传所有权。以上仅列入 [开发计划](RENDERING_PLAN.md)，其接口迁移继续遵守 [core 扩展方案](CORE_FORMAT_EXTENSION_PLAN.md)。

## 后续修复：缩放 / 平移时缓存恢复露底色

根因：`Tiles::prepare()` 每帧只恢复约 4 ms，`Tiles::draw()` 立即使用目标相机；缺失的目标 tile 被跳过，画布背景出现在图块位置。浅色主题下可表现为白色条块填充，不是第一次解码的正常行带呈现，也不是未初始化显存。

修复：viewer 分离目标相机与已呈现相机；首次解码保留原始行带呈现，但相机切换必须待目标可见区域完整就绪。恢复期间绘制旧视口并保护旧纹理，LOD 升级保留确切旧资源，只有最新目标成功后才原子切换并释放保护。取消 / 上传失败仍显示旧画面，不重新解码，不使用低清图替代原始 tile，不新增 shader / FBO。旧 / 新视口同时驻留仍受既有 GPU 预算；预算不足报错并保留旧视口，而非无界扩容。

验证：新增 `desktop.viewport_restore`，5 ms 上传延迟强制产生多帧缓存恢复，检查恢复中每一帧与上一完整画面一致。覆盖深 / 浅主题、透明 LOD 升级、目标连续变更、返回 FIT 取消、注入上传失败及再次操作恢复，原图提前删除，退出无纹理泄漏。本轮完整回归 **119 / 119 通过**；这不代表物理设备下缓存未命中时没有等待延迟。

## 后续优化：平移 / 缩放恢复时的停顿

代码检查确认此前相机恢复会在 UI 线程执行缓存 seek / read / zlib 解压，多处遍历完整 tile 表；保持旧画面避免了白块，但不能消除这些工作导致的等待。此次将交互读取 / 解压移入 CPU-only `tile_reader`，用独立 fd / pread、32 MiB RAM LRU、64 项队列和最新请求替换。图形 API 留在 UI 线程，完成通知唤醒，避免磁盘等待时主动空转。

可见 tile 通过源网格索引，GPU 淘汰扫描驻留集合，保留资源处理只访问实际保留集合；预热一圈邻居；FIT 下只在 RAM 预读 native 中心视口，不提前改变 GPU LOD 驻留。GPU 预热不抢占可见内容和准入余量。完整 tile 用事务式带像素创建，减少清零 / 覆盖及 mipmap 重复生成。失败保留旧视口；显式重试只清理失败的 RAM 结果，保留无关热缓存。

新增 4 个 reader 单测和 `desktop.interaction`：阻塞 worker 时 request / find 不执行 I/O；队列替换、RAM LRU、独立 fd 和 seek 不干扰、错误重试；原图删除后热邻居连续平移和同级缩放零新增读取 / 解压、零 GPU 重传；32K 回归校验相机访问范围不随整个 tile 表扩张。renderer 回归覆盖带像素创建的普通 / 非四字节 stride、状态恢复、立即释放源指针和失败回滚。完整回归 **124 / 124 通过**。

可复现交互命令：`SDL_VIDEODRIVER=offscreen ./build/dev/tests/liltview_viewer_sharpness --interaction`。本机一次 Debug / 512×512 离屏、4097×3073 易压缩单像素 PNG 测试（含 GPU 截图回读）中，预热平移 P50=3.586 ms、P95=5.013 ms；一次较远冷缓存切换 80.856 ms。此数据不是用户硬件的帧率保证，也不是受控的优化前后对比或对真实照片的性能结论；源码 codec 在交互中没有重新执行。

尚未解决：首次生成 LOD / 压缩 / 缓存追加仍在 UI，驱动单次 GL 调用可能超出调用间预算，冷缓存大跳转仍可能短暂保持旧视口。原有强缩小时的 atlas LOD 质量和全 app 资源总账仍是独立待办；没有通过降低清晰度或新增 shader 掩盖这些问题。

## 后续修复：恢复提示期间整个相机仍冻结

用户实测每次“正在恢复缓存纹理”都会卡住，不能将上一轮合成图热缓存性能当成此反馈已解决。代码中的 atomic 策略明确禁止未到最终视口前变更相机，这是显示层面的停顿，不只是后台读取的线程安排。网格查询使用带 decode/filter halo 的目标，实际在屏幕外的边缘 tile 也会被算进必需恢复项，进一步扩大等待。

本轮应用默认改为 covered-motion：目标视口未齐时，最多 8 次二分检查最近已呈现相机到目标间的仿射变换，只提交足够清晰且所有可见区域已驻留 GPU 的中间视口。每次推进仍绘制一个完整画面，不露背景、不用缩略图放大、不重新生成像素或重新解码；目标相机最终就绪后准确到达。物理像素覆盖检查去掉屏幕外 padding 的不必要等待，画布 clip 边缘也对齐物理像素。旧的 atomic 策略显式保留给原有故障 / 取消对照测试，应用默认不再选它。

回归：新增 `desktop.covered_motion`，以空间可识别的红/黄块、不透明 / 半透明两类输入和 5 ms 上传延迟，原图预先删除。旧实现可稳定失败“缓存未齐时相机仍完全冻结”；新实现验证首帧 / 后续平移与缩放的中间相机已推进、所有内部采样保持有效图像色值、位置对应相机、无双重 alpha 混合，并最终到达最新目标。viewport 单测在 DPI 1 / 1.25 / 1.5 / 2 和 EXIF 1..8 下验证 native 中心视口只需要实际可见的 4 块而非 halo 的 16 块，另测插值的缩放锚点。

本轮完整回归 **127 / 127 通过**，`git diff --check` 通过。限制：如果完整目标在显存中完全不连续、当前高清覆盖用尽，仍需保留最近画面等待，不可能在“不放大低清 / 不露空白 / 有界显存”的约束下保证任意冷跳转立即完成。单次 GPU 上传的驱动延迟、首次缓存生成和缓存 I/O 的真实设备时延仍须继续测量，不能仅凭状态栏文字判断所有剩余停顿来源。
