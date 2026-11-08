# LiltView Core API（当前 ABI 2）

- 状态：静态契约沿用 v0.1，运行时已迁移 ABI 2；Linux JPEG / PNG / SVG / WebP / BMP / GIF / TIFF DISPLAY / FIT 引擎已实现，支持范围见 §1.2。
- 实现语言：C23；调用侧：C、C++20 / C++23。
- 公共头文件：[core/include/liltview/core.h](../core/include/liltview/core.h)
- 相关设计：[总体设计](DESIGN-v0.1.md)、[工程落地](IMPLEMENTATION-v0.1.md)。
- 调用时序、所有权、生命周期及边界契约：[Core Runtime](CORE_RUNTIME-v0.1.md)。
- 新增格式与原始 RGB / YUV / Bayer 的 [接口 Review](CORE_INTERFACE_REVIEW-2026-11-08.md) 和 [兼容扩展方案](CORE_FORMAT_EXTENSION_PLAN.md) 已记录；其中候选输入 / 选择 / 颜色 / 输出 / 诊断描述均为待开发，不是当前 ABI 2 的声明，不改变本文件现有调用契约。

## 1. 设计边界

核心负责读取、解码 / SVG 栅格化、后台任务、预读、CPU 缓存与像素交付。核心不包含 ImGui、GPU 对象、窗口系统、翻译、spdlog 或更新检测依赖。

首版接口只接受本地文件，不提前引入网络数据源、解码插件 ABI 或编辑接口；已实现 GIF 动画会话和异步帧请求，详细契约见 [动画接口 v0.2](CORE_ANIMATION-v0.2.md)。格式枚举包含 JPEG、PNG、SVG、WebP、BMP、GIF、TIFF；实际编译支持通过 `lv_core_query_format()` 查询，不能以枚举存在代替实现验收。

### 1.2 当前实现范围

- 所有公共函数已有真实实现；query 对七类格式均报告静态显示可用，GIF 另报 ANIMATION；JPEG / WebP 为 SCALED_DECODE，SVG 为 VECTOR_RASTERIZE，PNG / BMP / TIFF 无额外能力位。不报告增量预览或随机区域能力。
- 仅 Linux 常规本地文件、DISPLAY / FIT、一个前台 worker。PREFETCH / REGION 请求同步返回 UNSUPPORTED；多前台 worker、非零 cache / disk cache 预算在 create 返回 UNSUPPORTED。配置初值保留，prefetch_workers 当前不创建线程，也不使预读可用。
- libjpeg-turbo 3.1.2，8 位 RGB / 灰度、baseline / progressive JPEG 最终结果、EXIF 1..8。CMYK / YCCK、12 位 JPEG 和超大图未接入；12 位 / 尺寸超过当前范围为 DIMENSION_LIMIT，颜色空间不支持为 FORMAT_UNSUPPORTED。
- PNG 的透明 / 索引色 / Adam7 / 高位深转换、静态 WebP、BMP 常见位深 / bitfields / RLE、SVG 基础静态 profile 已实现。GIF 支持合成首帧、TIFF / BigTIFF 支持普通尺寸首页，静态 submit 仍为首帧，UI 另使用 GIF 动画会话自动播放；TIFF 翻页未实现；APNG / 动画 WebP 拒绝；SVG 文字、裁剪、内嵌位图等未实现。详见[格式兼容表](IMAGE_FORMATS.md)。
- 位图文件最多 64 MiB；原始 `width × height × 4` 不超过 large_pixel_bytes 和 256 MiB 硬限制中的较小值，实际解码还须满足工作集预算。不因 FIT 输出很小而宣称支持超大源图。
- JPEG 先做缩放解码并以两行缓冲双线性生成完整 FIT CPU 图，再复制为最终 tile 通过有界队列交付；当前不发布解码中的行或渐进扫描。tile 可分帧上传，小图超时显示由 UI 计时器控制。
- 当前普通图计划预留 codec 保守 allowance、完整 FIT 图及其全部输出 tile 的潜在峰值。队列仍以单块 / 字节限流；这不是后续超大图的低内存扫描实现。第三方内部 / C runtime 分配不是 RSS 硬上限，accounted_cpu_bytes 不含未实际分配的预约。
- 当前 app 设置 CPU 384 MiB、队列 4 MiB、GPU images 128 MiB。它们是首版可调整工程初值，不是格式标准。INFO 可在 decode 前交付，实际像素在校验解码和源版本后发布。
- 收到 INFO 后再次修改文件的检测、取消竞争、单块背压、retained pixels、INFO 跨终结寿命和关闭有运行测试；预读 / 缓存 / REGION 条目仍是后续设计约束；四种新增格式也覆盖源修改与取消测试。

## 2. 核心对象与函数

| 对象 / 值 | 语义 |
| --- | --- |
| `lv_core` | 一组工作任务、预算、缓存和事件队列 |
| `lv_request_id` | 核心生成的非零、实例内不重复请求编号；耗尽时拒绝提交，不回绕 |
| `user_tag` | 调用方提供、事件原样返回的标记；可用于 UI 视图版本 |
| `lv_event` | 从队列转移给调用方的不可变事件，必须释放 |
| `lv_pixels` | 引用计数的不可变像素块，可脱离事件延长使用时间 |

| API | 用途 |
| --- | --- |
| `lv_core_config_init` / `lv_request_desc_init` | 初始化版本、大小和默认字段 |
| `lv_core_create` | 检查配置并创建引擎 |
| `lv_core_submit` | 接受不可变的显示或预读请求 |
| `lv_core_cancel` | 停止该请求发布新像素并协作取消 |
| `lv_core_poll_event` | 非阻塞取事件；无事件返回 `LV_EMPTY` |
| `lv_event_get_view` / `lv_event_release` | 读取事件并归还所有权 |
| `lv_pixels_get_view` / `retain` / `release` | 读取像素及延长生命周期 |
| `lv_core_query_format` | 查询构建提供的格式能力上限 |
| `lv_core_get_stats` | 读取内存、排队及借用对象统计快照 |
| `lv_core_begin_shutdown` / `join` / `destroy` | 停止、等待与销毁分离 |

不引入可变的 `update_view()`：切图、变更缩放输出或视口时提交新请求并取消旧请求。核心可复用同源缓存或解码工作，但不得改变请求、事件和预算的外部语义。自动重启同一请求解码不是时间阈值切换的实现方式。调用时序、终结竞争、跨线程所有权和错误边界以 [Core Runtime](CORE_RUNTIME-v0.1.md) 为准。

## 3. ABI 与初始化

- 公共头文件只依赖 `stddef.h`、`stdint.h`，使用 `extern "C"` 供 C++ 调用。
- ABI 字段使用固定宽度整数类型，匿名 enum 只定义常量；不把编译器 enum 大小、C++ 类或原子类型作为接口布局。
- `size_t` 只用于地址空间内存长度；不承诺跨 32 / 64 位进程共享结构，更不是磁盘序列化格式。
- 配置包含 `abi_version`、`struct_size`，请求包含 `struct_size`。当前 ABI 2 要求版本和结构大小精确匹配，不提供大小不匹配的“尽力兼容”。布局或语义不兼容变化需要提升 ABI 版本。
- 初始化函数将结构字段重置后填默认值；空输出指针为无操作。调用者使用与库匹配的头文件；动态加载不同版本时，应先查询 `lv_core_abi_version()`，匹配后再调用结构初始化函数。
- `create` 失败将 `*out_core` 置空；`submit` 失败将 ID 置零；`poll` 无结果或失败将事件置空。输出指针参数本身必须非空。
- 无效配置和请求返回 `LV_INVALID_ARGUMENT`；不满足 ABI 返回 `LV_ABI_MISMATCH`；同步失败不产生请求事件。
- C++ 异常不能跨越 C API 或回调边界；项目适配器自行捕获处理。

### 3.1 配置初值

| 字段 | 初值 / 要求 |
| --- | --- |
| `cpu_budget_bytes` | 初始化为 0；调用方必须设置非零预算 |
| `cache_budget_bytes` | 0，默认不保留已结束请求的缓存；启用预读必须非零 |
| `queue_pixel_bytes` | 初始化为 0；调用方必须设置非零预算 |
| `small_file_bytes` / `small_pixel_bytes` / `large_pixel_bytes` | 16 / 64 / 256 MiB |
| `progressive_after_ms` / `update_interval_ms` | 100 / 33 ms |
| `tile_edge` | 256，允许 16..1024 的 2 次幂 |
| `max_requests` | 16，包括尚未归还终结事件的请求 |
| `foreground_workers` / `prefetch_workers` | 1 / 1；前台至少 1，预读可为 0 |
| 磁盘缓存 | 预算为 0，禁用 |
| 日志 | WARN；空回调不输出 |
| 唤醒回调 | 空，调用方可自行周期性排空队列 |

除既有大小和时间阈值外，256 像素块、16 请求及线程数都是草案初值，后续基准可调整。这里没有擅自确定应用 CPU / GPU 总预算。

`cache_budget_bytes`、`queue_pixel_bytes` 都是 CPU 总预算内的限制，不是额外内存额度。配置必须满足它们不大于 CPU 预算、队列可容纳至少一块 `tile_edge × tile_edge × 4` 的像素、像素阈值有序，以及所有字节计算无溢出。实现还需计算并预留请求槽和控制事件开销；预算连这些开销都无法覆盖时创建失败。

磁盘缓存是可选实现能力，启用时要求绝对目录和非零配额；构建不支持时返回 `LV_UNSUPPORTED`，不能默默忽略。当前接口不承诺已实现磁盘缓存。

## 4. 文件路径与源身份

`lv_path` 显式携带编码、指针和字节长度，不要求结尾 NUL；UTF-8 / POSIX 路径禁止内嵌零字节，原生 UTF-16 路径禁止内嵌零代码单元。提交只做有界校验和内存复制，不读取文件、不解析头、不做同步 stat / realpath。

- `LV_PATH_UTF8`：严格有效的 UTF-8；按当前平台文件 API 转换。
- `LV_PATH_POSIX_BYTES`：Linux 原生路径字节，不强行转成 UTF-8；其他平台返回不支持。
- `LV_PATH_WINDOWS_UTF16`：原生 UTF-16 代码单元路径，字节数必须为偶数且数据满足对齐；POSIX 返回不支持。
- 首版接受绝对路径，UI 自行将用户的相对路径解析为绝对路径；解析和文件访问不依赖进程工作目录随后变化。
- 成功提交后可立即释放输入路径，核心已复制。拒绝提交不持有输入。

工作线程打开文件并建立源版本身份。`source_revision` 是实例内的不透明非零身份，非内容哈希、非跨运行持久标识。活动请求、未 release 的 INFO 或缓存保留身份记录期间，相同可观测文件身份及版本使用相同 revision；检测到变化则分配新值并失效缓存。身份记录全部释放后可淘汰，重建可分配新值，旧值不复用。UI 保留 FIT 的 INFO 作为版本锚点，REGION 的 INFO 与其一致才组合，详见 [Core Runtime §2.4](CORE_RUNTIME-v0.1.md#24-预览与细节的源版本)。不同 core 的值不能比较。读取中发现可检测的变化返回 `LV_ERROR_SOURCE_CHANGED`；普通文件访问不能保证检测所有并发修改，首版不承诺文件快照语义。

## 5. 显示请求与坐标

`lv_request_desc_init()` 默认 DISPLAY、CURRENT、FIT、user_tag=0；文件路径和非零目标尺寸由调用方填写。预读需显式设置 PREFETCH 类型和 PREFETCH 优先级。

### 5.1 FIT：适应窗口

`fit_bounds` 是经过方向变换后的物理像素边界，由 UI 将窗口尺寸乘以 DPI 得到。位图保持比例且不放大；SVG 按固有比例栅格化。`IMAGE_INFO.canvas` 返回实际未应用方向变换的输出尺寸，区域为整个输出画布。

FIT 不要求 UI 先知道原图尺寸，一次请求即可完成探测、适应计算和像素交付。方向旋转 90 度时，应先交换可用边界的宽高，再计算源坐标下的输出尺寸。

### 5.2 REGION：指定分辨率下的区域

调用方提供未旋转的完整输出画布尺寸 `canvas` 和其中一个半开矩形 `rect`。例如原图 12000 × 8000、原尺寸查看中心 1200 × 800 区域：

```text
canvas = {12000, 8000}
rect   = {5400, 3600, 1200, 800}
```

这只描述坐标空间，不要求分配 12000 × 8000 的完整缓冲。比例与源图自然比例应在一个输出像素的舍入误差内，否则异步返回目标不支持错误；不支持拉伸图片。没有可用自然尺寸或比例的 SVG 仅保证 FIT 路径，REGION 按栅格化能力决定。

REGION 的四个坐标和边长需无溢出、非空并落在 canvas 内。UI 将可见窗口区域逆变换到源方向坐标，再换算到 canvas，向外取整，必要时包含少量滤波边缘。

请求无法在预算内实现时返回错误，不能静默缩小 REGION 尺寸并仍报告成功。UI 可以保留先前 FIT 预览作为底图，随后提交更低分辨率或更小区域请求。对只能顺序解码的格式，区域请求可能需要扫描前面的数据；并不意味着随机访问已实现。

### 5.3 请求组合

同一图片可先有 FIT 预览，再有 REGION 细节请求。两者 ID 不同，UI 显式管理允许显示的请求集合，而不是只保存一个全局“最新 ID”。UI 保留底图的 INFO 事件作为版本锚点，细节的 source_revision 通过检查才叠加；终结和像素无需因此保留。细节请求提交失败时继续保留有效预览。

API 不提供隐式 replace 操作。UI 可先取消旧的视口任务以释放资源，再尝试提交新任务；已在 UI / GPU 内的旧像素仍可保留。`LV_BUSY` 时稍后重试，不在 UI 内循环等待。

## 6. 预读

- PREFETCH 使用相同的提交与取消入口，只允许 FIT 目标和 PREFETCH 优先级。
- 核心在后台生成适合显示的缓存，不发送 `REGION_READY`；成功先发送一次 IMAGE_INFO 再 COMPLETE，失败 / 取消可以没有 INFO，但仍有终结事件。
- CACHE=0 或 prefetch_workers=0 时拒绝预读，避免无缓存的无效工作。
- 预读 COMPLETE 表示目标结果已生成并按缓存规则处理，不能承诺后续不会被 LRU 淘汰；`final_tile_count` 固定为 0，因为没有向 UI 交付像素块。
- UI 提供目录顺序、下一张候选和浏览方向；核心不枚举或排序整个目录。
- 核心优先服务当前显示。重复源和目标可共享内部工作，但每个接受的请求仍有独立 ID、取消与终结语义。

## 7. 事件、像素和渐进呈现

正常运行期间，每个被接受的请求最终产生且只产生一个 COMPLETE、ERROR 或 CANCELLED。解析失败可以直接 ERROR；成功解析后先发一次 IMAGE_INFO，再发布像素。不同请求交错，同一请求 sequence 严格增加，允许因合并和取消出现间隙。

| 事件 | 语义 |
| --- | --- |
| IMAGE_INFO | 源尺寸 / 方向、实际文件能力、分档、输出 canvas 和完整请求区域 |
| REGION_READY | 区域像素、格网 tile_id、revision、是否 final 及呈现策略 |
| COMPLETE | 请求覆盖区域的全部最终块已经入队，生产结束，不等于 GPU 已显示 |
| ERROR | 错误码、系统错误、预算参数与可选诊断文本 |
| CANCELLED | 工作安全退出，不再发布数据 |

### 7.1 固定格网与覆盖

- 格网从 canvas 原点开始，tile_edge 由配置决定；最后一行 / 列及请求边界上的 tile 截取到请求区域。
- `tile_id = row × ceil(canvas.width / tile_edge) + column`，以 64 位受检运算计算；仅在请求内有意义。
- 同一 tile_id 的 rect 不变，内部 revision 从 1 开始严格增加；合并后调用方观察到的版本允许跳号，首个可见 revision 也可能大于 1。每次发布整个矩形的完整有效像素，不暴露未初始化行、未完成填充或未合并的交错 pass。
- 后续细化生成新不可变像素块，不修改旧块。final 标记后该 tile 不再修订。
- 最终块必须无缺口覆盖 IMAGE_INFO.region。DISPLAY COMPLETE 的 final_tile_count 是相交格网 tile 数，与最终交付对应；不能通过丢弃必要块来结束请求。
- 可合并尚未借出的同一 tile 中间版本，但不能影响已借出的数据。UI 若异步处理上传，也按 tile revision 防止回退。

### 7.2 小图与渐进模式的统一交付

所有模式都可采用分块上传。小图块标记 `LV_PRESENT_AT_COMPLETE`，UI 可先上传到未呈现纹理，等 COMPLETE 且上传完成后整体展示。UI 同时按配置中的 `progressive_after_ms` 设置单调时钟计时器；到期后可展示此前已上传的有效块，即使没有新的 REGION_READY 事件。核心也可将后续块标记为 `LV_PRESENT_IMMEDIATE`；同一请求呈现状态只向 IMMEDIATE 切换，不倒退。该阈值是显示策略，不承诺解码器能在阈值内产出像素。

UI 收到 IMMEDIATE 后可一并展示该请求此前上传的有效块。大图或渐进路径的块可以立即显示；块尚不具备完整有效像素时继续等待，不为满足时间阈值发布垃圾内容。若解码器完全不支持增量，中间结果仍可能晚到。UI 计时器和核心标记均不得重新启动解码来触发状态变化。

“完整后呈现”不需要核心积压整张最终图，也不要求 CPU 到 GPU 的上传一次完成。约 33 ms 的更新节奏是合并建议，单批仍受块大小、队列预算和 UI 上传预算限制。

### 7.3 所有权

```text
poll_event → 调用方拥有 lv_event
                 └─ region.pixels：事件持有一份引用
                       └─ UI 可 retain → 独立延长到 GPU 不再读取之后
event_release → 归还事件及其持有的像素引用
pixels_release → 归还额外 retain 的引用
```

- `get_view()` 返回借用指针，不能 free，不能修改。
- 事件中的 diagnostic 指针随事件失效，需要留存时复制。
- 像素可能有行填充；使用 stride_bytes 和 byte_count 验证范围。v0.1 RGBA8 为字节 R/G/B/A，非预乘 alpha，像素原点为左上角，方向由 UI 应用。
- 颜色转换未完成时报告 UNKNOWN，不把带未知配置的内容标记为已转换 sRGB。
- 不允许以 stack copy 的 event_view 延长内部指针寿命；使用 retain 或复制数据。
- retain 必须在已有引用仍有效时执行，不能从失效指针复活对象。

## 8. 队列、背压与线程

### 8.1 线程模型

core 创建所在的线程是该实例唯一控制线程，所有 core 控制函数、poll、query、stats、shutdown、join、destroy 均由该线程串行调用，不支持多个并发控制调用或回调重入。与调用时序和创建失败回滚有关的完整规则见 [Core Runtime](CORE_RUNTIME-v0.1.md)。

只读 view 可在持有对象期间跨线程使用。`lv_event_release` 和像素 retain/release 允许从其他线程调用，但调用方负责所有权移交；每个事件只释放一次，同一个引用不能重复释放。

日志和唤醒回调可能从工作线程或控制调用内触发，不持有供 API 使用的内部锁；回调必须短、不可重入核心、不可调用 UI、不可跨 C ABI 抛异常。userdata 保持有效直到 join 成功。

### 8.2 有界交付

- 提交时预留任务状态、路径副本和控制事件容量；槽位不足返回 BUSY，底层分配失败返回 OUT_OF_MEMORY。异步解码工作集另行准入；请求被接受不代表已经为其解码预留内存。
- `max_requests` 包含终结事件尚未 release 的任务，避免“任务结束但事件无限堆积”。
- 每个请求最多一次 INFO 和一个终结控制事件；其容量在接受请求时预留，不与像素队列争抢，不因像素队列满而丢失终结通知。控制池有界；终结 release 后仍借出的 INFO 继续占记录，新提交也可能因此返回 BUSY。
- 像素队列按字节限制；所有实际分配仍属于 CPU 预算。UI 借出后不占队列额度，但缓冲未释放前仍占总预算。
- 消费者长期 retain 可以耗尽总预算；核心等待可回收资源或协作取消，不无限分配。调用方需持续释放过期结果，不能一边等待完成一边永久持有所有临时输出。
- 对确定无法在预算中容纳的工作返回 MEMORY_BUDGET；对暂时由借用对象造成的不足，不伪装为永久不支持。
- worker 启动解码前必须为能继续推进的工作集及至少一个输出块作预算准入；优先级、缓存回收、预读让出和队列背压按 [Core Runtime §5](CORE_RUNTIME-v0.1.md#5-预算队列与进度保证) 执行，不得让内部任务互相等待其仍占用的工作集。
- 缓存与借用可能共享同一分配，统计字段可能重叠，不能把各字段简单相加作为 RSS。未纳入分配追踪的第三方临时内存仍需另行峰值测量。

### 8.3 唤醒协议

队列从空变为非空时发出合并唤醒；会话槽、请求槽、控制记录和像素资源归还时也安排合并唤醒，允许此时 poll 返回 EMPTY；worker 全部退出时也唤醒控制线程。回调只投递平台事件，不读取核心队列。允许多余唤醒。

UI 每次唤醒后持续 poll 到 EMPTY，或在达到每帧预算后自行安排下一帧继续排空。队列仍非空时不能完全依赖新的边沿通知，否则可能丢失后续消费机会。发布与唤醒需保证消费者检查 EMPTY 和进入平台等待之间不会丢失通知。

## 9. 取消和关闭

### 9.1 取消竞争规则

- 取消与终结提交以同一同步判定点竞争。活动请求 cancel 成功后，不再发布新非终结事件，移除其尚未 poll 的像素事件，保留已排队 INFO，并唤醒处于背压等待的工作任务。
- 已 poll 的事件和 retained pixels 仍有效，但 UI 立即将该请求移出可展示集合。
- COMPLETE / ERROR / CANCELLED 终结若已经提交，cancel 返回 OK 且为无操作；保留该请求全部已排队事件，确保 COMPLETE 不会与其最终块分离。UI 仍按自己的请求集合过滤旧结果。终结 release 后请求槽释放，之后 cancel 返回 NOT_FOUND。
- 活动任务接收到取消后，到达安全退出点再发 CANCELLED。取消不强杀线程，不保证第三方阻塞调用立刻返回。
- 对仍占请求槽的请求重复取消返回 OK；槽位已随终结事件释放或 ID 从未存在则 NOT_FOUND。

### 9.2 三阶段关闭

1. `lv_core_begin_shutdown()`：非阻塞进入 closing，拒绝新请求，取消任务，丢弃未 poll 事件并解除背压。重复调用返回 OK。此阶段放弃逐请求终结交付承诺，防止关闭必须先把队列完整消费才能继续。
2. `lv_core_join(core, timeout_ms)`：等待所有 worker 和回调退出；0 表示只检查，未退出返回 TIMEOUT。未开始关闭返回 INVALID_STATE。UI 应以 0 或较小等待结合事件循环，不长时间阻塞窗口。
3. 归还借出的事件和 retained pixels，再调用 `lv_core_destroy(&core)`：未 join 或仍有外部对象时返回 BUSY 且保留指针；成功释放并置空。重复销毁空指针值可返回 OK。

begin_shutdown 之后 poll 返回 CLOSING，调用方只能处理此前已借出的对象；stats、join 和 destroy 仍可用。join 成功保证不再发生核心回调，但 retained pixels 可以继续有效，直到归还并销毁核心。完整的回收顺序、超时和并发访问约束见 [Core Runtime §6](CORE_RUNTIME-v0.1.md#6-关闭与回收时序)。

## 10. 最小调用流程

```text
config_init → 设置 CPU / 队列预算、可选缓存、日志与唤醒
create
request_init → 设置文件、FIT 边界
submit → 保存 request_id

平台唤醒 / 帧循环：
  poll_event
  按 request_id 判定是否仍可展示
  INFO：检查源版本并建立画布；底图 INFO 保留作版本锚点
  REGION：读取 / retain 像素，按预算上传
  COMPLETE：等待最后上传完成，再标记显示完成
  ERROR：按错误码翻译提示
  release_event（版本锚点 INFO 在底图废弃时归还）

切图 / 新视口：cancel 旧任务 → submit 新任务
相邻图片：submit PREFETCH → 收终结事件并 release
退出：begin_shutdown → 非阻塞检查 join → 归还外部对象 → destroy
```

## 11. 验证与后续实现

调用时序、内存所有权、生命周期与边界行为的实现基线和验收矩阵见 [Core Runtime](CORE_RUNTIME-v0.1.md)。若本文摘要与 Runtime 契约冲突，以后者为详细规则，并在实现前同步修正头文件注释。

已有 C23 / C++20 / C++23 ABI 契约检查，以及七类静态格式、事件 / 像素 / 源版本 / 预算 / 取消 / 关闭运行测试。仅头文件检查不能代替运行测试，当前具体范围以 §1.2 为准。

接口设计阶段已检查：GCC/G++ 下三个标准模式均以 `-Wall -Wextra -Werror -pedantic-errors` 编译通过；14 个公开结构的大小与对齐一致，17 个 API 引用均为未经过 C++ 名字修饰的符号。临时验证程序已清理。

CMake 已接入各格式静态依赖和完整引擎入口，不能以历史初始化阶段的 3/3 或 2/2 计数代表当前功能验收。构建与测试范围见 [项目目录与构建](PROJECT_STRUCTURE.md)。

实现阶段使用 Catch2 覆盖：

- 接受 / 拒绝请求时的输出值、结构版本和配置验证。
- 小图超时转换、最终覆盖、tile revision、INFO 与终结顺序。
- 取消与完成竞争、取消背压任务、预读不发布像素。
- 保留像素后释放事件、异步释放、shutdown 时有借出对象。
- 控制事件预留、请求槽回收、连续快速提交及内存预算。
- FIT 方向变换、REGION 坐标、SVG 无自然尺寸与格式能力差异。
- UTF-8、Linux 原生字节路径、Windows UTF-16 与跨平台不支持情况。

不在当前阶段提供假解码实现、空引擎函数桩或宣称稳定二进制兼容。解码库、磁盘缓存和平台后端选定后，应据实际能力审阅本接口，再冻结第一份引擎实现版本。
