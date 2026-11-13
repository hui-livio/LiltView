# 性能测量与测试计划

本文集中维护基准运行方法和测试矩阵；未接入的诊断阶段仍为待采集，不代表指标已达标。输入说明见[样本清单](inputs/README.md)，待开发能力见[开发计划](../docs/开发计划与未实现设计.md)。

## 运行基准

手动真实图片基准不加入 CTest，也不要求把个人文件提交到仓库。以下命令在仓库根目录执行，要求 Linux、启用全局菜单及可用的 SDL_Render 环境：

```sh
cmake --preset app-release -DBUILD_TESTING=ON -DLILTVIEW_BUILD_DESKTOP_TESTS=ON
cmake --build --preset app-release --target liltview liltview_viewer_benchmark
build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
LILTVIEW_BENCH_DIRECT=1 build/app-release/tests/liltview_viewer_benchmark /path/to/large.png 1600 1000
```

基准记录实际 SDL_Render 驱动、swap interval、pump / prepare / upload / submit / present、帧分位数、目标完成延迟、读取 / 恢复和资源计数。隐藏窗口、关闭 VSync，不含截图读回或应用菜单；帧间测试节流计入阶段总时间，不计单帧。具体矩阵与采集口径见下文。

## 当前输入与新增范围

现有 62 个棋盘格随机色块样本及校验值见 [inputs/README.md](inputs/README.md) 和 [manifest.json](inputs/manifest.json)。输入完整性核验不等于应用兼容性／性能验收，图片文件本身不随 Git 克隆提供。SVG 样本仅覆盖基础几何元素数量；复杂 SVG 当前不支持，不纳入成功渲染或吞吐成绩，可单独记录显式拒绝行为。

[开发计划](../docs/开发计划与未实现设计.md) 的新增格式均纳入本矩阵。它们的实现状态为 **待开发**，输入素材为 **待补充**，性能结果为 **未测量**；没有把未来文件写入现有实物样本清单，也不增加虚假的样本数量。

| 测试标识 | 新增格式／能力 | 必测场景及分组 |
| --- | --- | --- |
| `avif` | AVIF | 静态有损／无损、alpha、高位深；普通尺寸动画另组 |
| `jxl` | JPEG XL | 静态有损／无损、alpha、高位深、渐进首屏与最终画面；动画另组；读取 JPEG 无损重压缩输入 |
| `heif` | HEIF／HEIC | 主图、方向、高位深及已声明兼容编码；集合／序列能力单列 |
| `apng` | APNG | 首帧与后续帧、透明、局部更新、帧合成、可变时长和循环 |
| `webp-animation` | 动画 WebP | 同源 GIF／APNG 对比，固定与可变帧时长、透明合成、循环 |
| `icon` | ICO／CUR | 多尺寸条目选择、PNG／DIB 条目、透明掩码、CUR 热点；不强行套用 100MP 超大图 |
| `qoi` | QOI | RGB／RGBA，无损 PNG／WebP／TIFF 同源对比；普通与适用的超大静态图 |
| `jpeg2000` | JPEG 2000 | JP2／J2K 支持子集、有损／无损、高位深、全图与具备能力时的区域访问 |
| `openexr` | OpenEXR | 半浮点／浮点、已支持压缩、scanline／tile、通道选择与显示映射 |
| `camera-raw` | 相机 RAW／DNG | 内嵌预览和完整显影分别测量；固定曝光、白平衡、去马赛克和输出质量 |
| `raw-rgb` | 原始 RGB／灰度 | 通道顺序、位深、字节序、紧密／padding 行、非零数据偏移、适用的高精度输入 |
| `raw-yuv` | 原始 YUV | 4:4:4／4:2:2／4:2:0；I420／YV12、NV12／NV21、YUYV／UYVY／YVYU／VYUY、NV16／NV61、NV24／NV42，以及明确布局的 packed 444、高位深变体 |
| `raw-bayer` | 原始 Bayer | RGGB／BGGR／GRBG／GBRG；8／10／12／14／16 位，非打包与具体 packed 规则；解包、去马赛克和颜色处理 |

所有适用样本都采集完整的公共指标，不只测试格式表中的特色场景。原始输入参数和 YUV／Bayer 变体见[原始像素计划](../docs/开发计划与未实现设计.md#3-无文件头原始像素)。`YUY2`／`IYUV` 等别名核对布局后与对应格式关联；用户提到的 `UYUV` 暂按 `UYVY` 登记，不生成未定义格式。

## 图片类别与素材要求

- 普通静态图：沿用约 2／12／24／48MP；每格式按自身限制选取合法尺寸，图标另用多尺寸条目组。
- 超大静态图：沿用 100MP／400MP／1GP 与超长图，对支持该尺寸且符合预算的格式开展。格式上限、当前未实现或无法生成的组合明确记录原因，不以改扩展名制造样本。
- 普通动图：480p／720p／1080p，覆盖帧数、帧时长、透明、合成及循环；不要求超大动图。
- 原始像素：普通尺寸、4K、适用的超大静态图；交叉覆盖具体布局、位深、stride、plane offset、文件 offset、字节序、颜色参数及 Bayer CFA。
- 主对照使用固定种子的随机色块棋盘；同源文件建立 comparison group。块密度、alpha、编码参数、输出尺寸和色彩解释不同的组分开比较。
- 另补真实内容、灰阶／饱和色、布局边界和已知 Bayer 小图；棋盘格易压缩，不代表所有照片与传感器数据的复杂度。
- 所有新增样本先通过独立解析／解码检查，记录 SHA-256、源尺寸、格式变体及生成工具版本。高位深不能仅改标签；YUV 必须记录从 RGB 转换的规则；Bayer 必须实际进行 CFA 采样。

## 打开与持续显示流程

分别测“新进程启动并打开”和“程序已运行时打开”。记录请求起点、首个原始行带、全图完成、当前视口达到目标质量的时间。超大图的 FIT 是原始场景显示方式，需另测全图就绪、100% 冷纹理恢复和持续交互，不能当作 FIT→REGION 重解码流程；动图首帧与持续播放分开。

缓存条件分为新进程文件冷缓存、新进程文件热缓存、同进程重复打开；进程重启不等于文件冷缓存。每个样本每种打开条件先测 30 次，报告 P50／P95／最大值；持续显示／播放先测 5 轮。超时、拒绝、崩溃和成功率全部记录，不只保留成功轮次。

静态图完成后等待 5 秒，观察 30 秒；普通动图预热 5 秒后观察至少 60 秒且覆盖 3 轮，有限循环另测自然结束。同进程交替打开固定样本 100 次，比较预热后的资源趋势，再切回固定小图观察残留。算法、输出质量或布局改变后另立测试组。

## 分段计时

| 阶段 | 记录范围 |
| --- | --- |
| 排队 | 请求收到到 worker 开始处理 |
| 文件读取 | 文件打开／读数据，注明独立阶段还是与解码交错 |
| 压缩解码 | 解码器初始化与像素产生，另记累计 CPU 时间；不能拆开的解码＋缩放明确合并 |
| 原始数据处理 | RGB 重排／解包；YUV 解包、色度重建与颜色转换；Bayer 解包、去马赛克、白平衡／颜色处理分别计时 |
| 像素处理 | 缩放、方向、像素格式与显示映射，避免与上一阶段重复记账 |
| 交付等待 | 数据可交付到 UI 消费，定位队列背压与主线程延迟 |
| 纹理准备／上传 | 创建与复用分开，记录 CPU 提交耗时；只有可靠识别传输工作时才另报 GPU 传输时间 |
| 绘制 | CPU 构建／提交与 GPU 执行分开，图片自身和含 UI 的整帧分开 |
| 呈现 | 交换缓冲调用等待与屏幕实际可见延迟分开；前者不能代替后者 |

viewer 已有 pump / prepare / upload / submit / present 分段；首次 TilePreparation 另记 pack / LOD / 压缩 / 缓存写入和队列高水位。源 I/O / 纯解码 CPU 与 GPU 完成时间线仍需细化。无埋点阶段标记未采集；外部摄像判定画面时注明帧间隔误差，不能用“解码完成／图片 displayed”日志代替真正可见。

解码专项提前准备压缩输入，排除磁盘、UI 和 GPU；实际加载组仍走真实链路。吞吐量同时注明源像素、实际解码像素和输出像素，使用实际解码总量／总解码时间，缩放解码不以完整源图像素夸大吞吐量。

**原始 RGB／YUV／Bayer 的压缩解码填“不适用”**，仍报告读取、解包／转换／去马赛克的 ms、CPU ms、实际处理 MP/s；需要时另记源读取 MiB/s。相机 RAW／DNG 如涉及压缩解码，则将压缩解码与完整显影分段记录。不能把无压缩解码解释为零处理成本。

GPU 绘制计时使用适用的 GPU 时间查询／分析工具，延后读取结果；不在每个测量点强制全局同步。专项测试分别运行纹理已准备、首次上传、持续更新三种场景。VSync 开／关分组，正常体验组保持实际配置。

原始 YUV／Bayer 如提供 CPU 与 GPU 处理路径，两条路径单独报告转换或去马赛克所在阶段、资源与画质；GPU shader 内同时转换与绘制时明确报合并耗时，不虚构独立阶段。Bayer 去马赛克算法、输出尺寸、颜色处理参数必须相同才比较实现性能。

阶段可能重叠；保留同一请求／帧的时间线，不能相加各阶段 P95 推算端到端 P95。

## CPU、内存与 GPU

| 指标 | 打开阶段 | 持续显示／播放与重复打开 |
| --- | --- | --- |
| CPU | 平均、采样峰值、累计 CPU ms；约定一个逻辑核满载为 100% | 平均、P95、峰值和随轮次变化 |
| 内存 | RSS 峰值、空窗口基线与增量；工作区按阶段辅助解释 | RSS／PSS 稳态与趋势，核心账本和进程实测分开 |
| GPU | 应用相关引擎平均／采样峰值、纹理／显存峰值 | 引擎平均／P95、显存稳态与趋势；转换 shader 的工作计入 |
| 动画质量 | 首帧耗时 | 实际帧时刻、迟到、旧帧重复／丢帧、停顿与播放时长偏差 |

目标 CPU／RSS 采样间隔 100ms，PSS 1s；GPU 记录真实采样能力，短于窗口的峰值注明可能遗漏。进程生命周期内的内存高水位不能直接当作同进程某次打开的独立峰值。应用／子进程统计范围、GPU 引擎归属、设备／共享内存口径写入报告；只能取整卡指标时标注整卡与空闲基线，不能冒充应用独占消耗。

## 比较条件与结果记录

同机器、同文件、同输出尺寸与质量、同配置，对基准版本和待测版本交替测量。记录构建／提交版本、CPU／内存／GPU／驱动、存储、桌面会话、屏幕与窗口大小、缩放、VSync、日志级别，以及所有原始像素参数。HDR／SDR、内嵌预览／完整显影、CPU／GPU 转换分别比较。

每样本报告：打开状态与成功率、首画面／目标视图完成 P50／P95、解码或原始处理 ms／吞吐量、上传 CPU ms、绘制 CPU／GPU ms、呈现等待、CPU／RSS／PSS／显存／GPU 引擎数据及相对基准变化；附典型慢请求／慢帧时间线。

状态必须区分：`待补充样本`、`待开发`、`未测量`、`通过`、`失败`、`不支持`、`不适用`、`不可采集`。缺失值不填零。新格式未实现前可以测拒绝延迟和拒绝期间资源，但不能把它当作成功打开、解码或渲染成绩。

先建立参考基线再确定绝对目标。退化判断结合相对变化、预先确定的绝对容差和复测波动；播放质量或输出质量下降时不能以资源更低作为性能提升。

## 接口扩展的补充验收（待开发）

Core COMPLETE 总耗时不足以提供全部分段数据，接口与诊断补充见[开发计划](../docs/开发计划与未实现设计.md#4-core-接口扩展边界尚未实施)。以下为后续采集 / 验收要求，不代表当前 API 已提供这些字段或已经生成测量结果。

- 每轮保存文件版本、实际选中 item / 预览角色、raw layout、颜色 / 处理身份、实际输出布局及后端；同文件不同参数或预览 / 完整显影不混组。
- 将 raw unpack、YUV 转换、Bayer 去马赛克 / 颜色处理、预算 / 队列等待纳入有界诊断，成功、失败、取消都保留有效字段与 unknown 标记。
- 用 request / processing / frame key 关联 core、viewer、renderer 与屏幕观测；CPU 与 GPU 未校准的时钟不能直接相减，core COMPLETE 不能代替可见终点。
- 比较整图和 ROI 拼接的颜色 / 相位 / 边缘正确性，再比较速度；固定去马赛克、tone-map 和质量模式，不能通过降低精度冒充提速。
- 增加高精度 / 多平面最小可交付块、低队列额度、第三方工作集、metadata / trace 开销测试；验证预算不足明确失败而非永久等待。
- 诊断启用 / 关闭分组测量采集开销，trace 溢出有丢弃计数，不挤占必达终结事件，也不靠全局 GPU 同步制造失真的阶段数据。

## 8K 首次加载：流水与集中上传对照

使用 [loading_compare.py](loading_compare.py) 生成固定种子的 8192×8192、16px 随机彩色棋盘格 PNG。相邻格明暗交替，每格 RGB 独立随机，512px Core tile 不是纯色；脚本实际解码检查并记录 SHA-256、尺寸、生成器与机器信息。生成文件和原始日志位于 `build/`，不覆盖 `assets/`。

```powershell
# 在 x64 MSVC 开发环境中
cmake --preset msvc-release
cmake --build build/msvc-release --target liltview liltview_loading_benchmark --parallel 6
python benchmarks/loading_compare.py --exe build/msvc-release/app/liltview_loading_benchmark.exe --runs 30 --output build/loading-comparison-8k
```

每个样本启动新进程，交替两种模式顺序，使用真正的 YAML 解析；输入 OS 文件缓存预热，匿名 tile 缓存每进程重建，不声称冷磁盘。默认 VSync 请求开启、1600×1000 隐藏窗口，并明确以 16.667ms 目标帧周期调度，模拟帧预算运行条件；隐藏窗口的 VSync 不等于真实前台显示行为。`--vsync 0 --cadence-ms 0` 可单独测不限制帧周期的吞吐，不与默认组混算。

输出每样本 JSON、帧级 CSV、阶段日志和 summary.json。报告首屏 / present 返回、上传片段峰值、主线程 work 分布、5ms 探针事件的处理延迟、暂存高水位、进程 CPU 与峰值 RSS。事件延迟包含调度等待；最终上传突发后的排队探针也会被排空采样。RSS 是进程高水位，含初始化和最终画面读回，不冒充仅加载缓冲的精确账本。

验收自动检查：pipeline 首次上传早于观察到的 Core COMPLETE；batch 在 Core / worker 结束后才首次上传；每种模式 256 个 tile、无半成品可见状态、输入保留不超过两块，以及最终 SDL 读回 RGB 的逐字节 FNV-1a 校验值全部相同。完整产品 UI 行为另由 Dock / Shell / navigation render 回归验证。

已完成的 30+30 主组与 10+10 吞吐诊断组数据见 [2026-11-13 实测报告](results/2026-11-13-first-load-8k/README.md)。

已有本地图片可直接使用 `liltview_loading_benchmark IMAGE SETTINGS_YAML REPORT_JSON [VSYNC=1] [FRAME_MS=16.667] [SCREENSHOT_PPM]`，可选截图在首屏计时结束后保存；不要使用会生成固定 8K 输入的 Python runner 代替任意图片测试。

## 内存缓存压缩开启 / 关闭对照

使用同一个 Release `liltview_loading_benchmark`，只修改隐藏配置 `static_cache_compression`。Windows 示例：

```powershell
python benchmarks/cache_compression_compare.py --exe build/msvc-release/app/liltview_loading_benchmark.exe --output build/cache-compression-4k-8k-16k --source D:/share/images/1moban.png --runs 5
# 已有输入时使用 --inputs <包含 4k.png / 8k.png / 16k.png 的目录>，不再传 --source
```

`--source` 需 Pillow，并要求本地可信源至少 15360×8640，最多 240MP；生成居中、不缩放的 3840×2160、7680×4320、15360×8640 PNG。个人图片及裁图只留在 build，不提交。每组使用独立进程，串行交替开关顺序，不主动清 OS 文件缓存，记录实际 driver / VSync / cadence。不要同时运行构建或其他基准。

输出逐轮 JSON / frame CSV / 日志和 summary.json，校验两模式 framebuffer hash 一致、无半成品显示、worker / 队列有界、关闭模式压缩与解压时间均为零。主要比较完整首屏 ready / present 返回，另记录 FIT 60 帧、100% 恢复与后续 30 帧、峰值 RSS、容量和阶段累计。`load_peak_rss_bytes` 在渲染探针前采样；`peak_rss_bytes` / CPU 包含后续探针。worker 阶段时间可重叠，不能相加解释端到端时延。

当前同源 PNG 开关结果见 [2026-11-13 报告](results/2026-11-13-cache-compression/README.md)。这不是旧版 zlib / 磁盘实现与新版的同条件 A/B；也不代表所有格式、不同硬件或冷磁盘秒开。
