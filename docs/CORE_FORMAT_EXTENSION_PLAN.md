# Core 格式兼容接口扩展方案（待开发）

日期：2026-11-08。依据 [Core 接口 review](CORE_INTERFACE_REVIEW-2026-11-08.md)、[格式待办](FORMAT_ROADMAP.md)、[原始像素方案](RAW_PIXEL_INPUT.md) 和 [性能计划](../benchmarks/TEST_PLAN.md)。本文是建议的字段与契约，不是已经存在的 C 声明；不修改当前 `core.h`，不提供假实现。以下候选描述名只用于设计讨论，最终类型名、数值与布局在实施前冻结。

## 1. 扩展策略与 ABI

保留“本地文件 → 不可变请求 → worker → 不可变事件／像素”的主线。首轮原始数据仍来自常规文件，调用方传描述；不同时引入网络流、用户 I/O 回调、DMA-BUF、相机采集、任意内存借用或动态插件 ABI。

仅增加新文件格式常量、内部解码器或复用 RGBA8 动画适配器，不天然要求改变结构布局；但 raw 输入、选择器、输出／颜色描述等需要布局变化，建议集中形成 **下一主 ABI（建议 ABI 3，尚未实施）**。届时保留已有常量含义与值，不在 ABI 2 结构尾部偷加字段，不把 reserved 字段改成未声明的指针。

下一主 ABI 沿用本平台架构内固定宽度字段、精确 struct_size 校验和匹配头文件规则；配置／请求及可独立取得的复杂 view／扩展描述具有明确版本与大小。所有消费者先查询 ABI 再初始化，整体更新初始化器、event union、C23／C++20／C++23 布局与运行契约测试。未知必需格式／选项明确拒绝，不能静默忽略。若将来要求 ABI 2 与 3 同时加载，另做显式版本化入口和完整适配，不宣称只替换共享库即可兼容。

## 2. 请求描述拆分

| 候选描述 | 必要内容 | 语义 |
| --- | --- | --- |
| `input` | 编码文件／原始像素文件种类、本地路径、可选已验证格式提示 | 路径扩展名不是 raw 布局；编码提示必须与内容核对 |
| `raw_layout` | 宽高、具体存储格式、plane 表、offset／stride、位深／类型／packing／字节序 | 描述文件里的字节，不能用显示 target 表示源尺寸 |
| `selection` | 默认主图或明确 item／page／part／通道映射；相机预览／完整显影角色 | 一个选择器不能同时表示静态条目与时间帧 |
| `processing` | YUV 颜色解释、Bayer 参数、曝光／白平衡／去马赛克／显示映射及质量模式 | 不可变处理配方，与存储布局分开 |
| `output` | 允许的像素格式、目标颜色、alpha 语义、精度／HDR 策略、是否允许明确降级 | 请求的输出策略；INFO 必须回报实际结果 |
| `target` | 现有 FIT／REGION 和方向契约 | 继续只表达显示空间和目标覆盖 |
| `diagnostics` | 是否采集阶段统计、可选预算内 trace 等级 | 默认轻量、不能强制 GPU 同步 |

`processing` 不暴露第三方库结构／未定义键值字典；采用有版本的类型化描述，限制平面数、通道映射数、profile／参数字节数，首轮 raw plane 最多 4 个。存在变长数组／字符串／ICC 时，submit 成功前必须有界复制全部会被异步使用的值并计预算；不能浅拷贝指针后要求 UI 活到解码结束。未知必需字段拒绝，默认值规范化并回报。submit 保持不做文件 I/O；只做有限的描述／算术校验。

配置文件和原始像素设置面板位于应用层。core 接收规范化描述，不解析 UI YAML、不读取一个隐式 sidecar；配置文件改变时应用重新加载并提交新请求。编码文件自动探测与强制 raw 解释分为明确模式，不能因 raw 缺少 magic 就无条件拒绝，也不能自动把未知文件当 raw。

## 3. 原始布局及解释

| 类别 | 输入字段与校验重点 |
| --- | --- |
| RGB／灰度 | 通道顺序、整数／浮点类型、有效位与容器位宽、字节序、alpha／行方向；RGB565 掩码／字节序不能省略 |
| YUV | 4:4:4／4:2:2／4:2:0、planar／semi-planar／packed、UV 顺序、每 plane 尺寸／stride／offset、矩阵／范围／色度位置、色域／传递函数 |
| Bayer | RGGB／BGGR／GRBG／GBRG、CFA 原点定义、有效／存储位深、具体 packing、黑白电平、白平衡、去马赛克及边界质量策略 |

具体 YUV 格式包含 I420／IYUV、YV12、NV12／NV21、YUYV／YUY2、UYVY、YVYU、VYUY、NV16／NV61、NV24／NV42、明确排列的 packed 444 及高位深变体。`YUV420`、`YUV444`、`RAW10` 不能唯一标识字节布局；P010 类存储与 10 位 planar 也不能仅凭位深互换。`UYUV` 暂按用户所指 UYVY 记录，解析时拒绝未定义名字。

文件／plane offset 与 stride 用受检 64 位字节量；正 stride 与显式 row order 分开，不用无符号字段编码负步长。按格式推导 plane 有效宽高／行字节；奇数尺寸、packed 尾组、对齐限制逐格式定义。声明的尺寸合法但实现不支持时明确拒绝，不截断。

worker 打开文件后，以 `base_offset + plane_offset + (plane_rows−1)×stride + active_row_bytes` 检查每个 plane 的读取上界。各项先做乘加溢出检查，最后一行不强制存在不会读取的 padding；不同 plane 的重叠仅在明确定义的布局中允许。多帧 raw 后续另加 frame_stride／frame_count，不能从剩余字节自动猜动画。平台文件定位必须能够表示实际 64 位偏移，不能先窄化成 long／size_t 再检查。

Bayer CFA 以 **规范化行方向后、未应用显示方向的完整源图原点** 为参照，记录需要的传感器／active-area 原点映射；ROI 以全局源坐标计算相位，不把每块左上角重新当 RGGB。相机 RAW 适配器负责从文件元数据归一 active area、黑白电平和 CFA；不能把相机厂商私有 metadata 原样作为公共 ABI。

Bayer 参数应能区分 R／Gr／Gb／B 四个 CFA 位置的黑电平，明确白电平／增益的通道映射，以及颜色矩阵的输入／输出空间和单位。更复杂的行列校正、镜头阴影或非 2×2 CFA 不隐式归入四种 Bayer；在对应子能力实现前明确报告限制。去马赛克算法及质量版本进入处理身份，不能只记录“已去马赛克”。

缺少颜色／标定参数时区分“必需参数缺失”和“允许明确假设的预览”。只有调用方允许时才采用具名预设，并在实际结果中报告假设；不得按分辨率静默猜 BT.601／709，也不把任何 10 位数据直接标成 HDR。

## 4. 像素输出、颜色与 renderer

2026-11-09 更新：独立图像 shader、RGB / YUV 原生上传列入 [渲染开发计划](RENDERING_PLAN.md)，本轮不实现。当前仍输出 RGBA8；下述 CPU 兼容阶段与未来高精度 / 多平面阶段不能混称为已支持。后续 GPU 路径须显式协商 plane / 颜色 / 所有权和 ABI，而非把 YUV 字节装入现有 RGBA8 描述。

### 4.1 第一阶段：兼容现有显示链路

原始 RGB 重排、YUV→RGB、Bayer 去马赛克、相机显影及 EXR 选通道先在 CPU 完成，输出 RGBA8。高位深／HDR 输入必须经过声明的量化／显示映射策略；不能简单截高低字节，也不能把 SDR 预览宣称为保留高精度或原生 HDR。

新增源信息至少记录实际 codec／variant、源通道／位深／sample type、alpha、源颜色（ICC／CICP 或具名未知状态）及选择角色。输出描述记录实际格式、颜色、alpha、方向状态、映射／降级标记和处理身份。只在转换确实完成时声称 sRGB；未校准 Bayer、缺失 YUV 参数和未知 profile 不能凭默认枚举变成准确颜色。

core 不持有窗口、显示设备或 GPU 对象。应用可传有界目标 profile／具名目标颜色策略，由 core CPU 转换；renderer 按实际输出解释。避免重复转换／重复 tone-map，显示目标改变须更新处理身份与请求。

### 4.2 后续阶段：高精度／多平面与 GPU 转换

候选输出先考虑 RGBA16 整数、RGBA16F／32F，再按实际需求接入 NV12／P010 等原生 plane；不是所有源布局都必须原样暴露。renderer 的 image／upload／capabilities／readback 必须同步扩展像素格式、采样、颜色、alpha 与内存计量，并查询设备真实组合能力；当前 RGB8 readback 不能验证 HDR 保真。

新像素 view 描述 plane 数量、plane 的 data／可读 byte_count／stride／样本尺寸／格式和可用 crop。所有 plane 和颜色描述由同一个不可变像素对象拥有或锚定，retain 像素后即使 INFO／event 释放，plane 指针及必要颜色数据仍有效；沿用关闭和最终 destroy 等待外部引用的规则。

输出需要能力交集：`该输入／处理可生成的布局 ∩ 调用方允许 ∩ renderer 可消费`。没有交集且未显式允许转换回退时报告不支持；允许回退则 INFO 回报实际路径与降级。core 只交付 CPU 数据及元数据，GPU 转换归 renderer，不把 GLuint／D3D／窗口句柄穿过 C core。renderer 同步消费／自有 staging 的现有寿命规则保持；若未来异步保留输入，另明确 retain／完成与 staging 预算，不能隐式延长裸指针读取。

renderer 的 linear／nearest 放大采样属于绘制状态，不是输入存储布局；原始像素检视可选择 nearest，性能对照必须记录并固定采样方式。它不等同于 core 解码／去马赛克质量选项，也不意味着已增加 YUV／Bayer shader 支持。

## 5. 文件身份、解释身份与缓存

保留 `source_revision` 为 core 实例内的文件版本身份，不把参数偷偷编码进该字段。规划新增：

| 身份 | 包含的语义 | 用途 |
| --- | --- | --- |
| 文件版本 | 当前可观测文件身份与修改版本 | 源变更检测，不承诺跨运行 hash 或文件快照 |
| 选中图像身份 | 文件版本＋item／page／part／预览角色／通道映射 | 区分同文件不同内容 |
| 处理／解释身份 | 选中图像＋raw layout／颜色／CFA／packing／显影与质量配方＋输出颜色／精度策略 | FIT 与 REGION 是否可叠加、像素语义是否相同 |
| 具体缓存键 | 上述身份＋目标 canvas／区域／层级／实际输出布局及实现版本等 | 区分不同分辨率／区域的缓存项 |

请求 ID 仍只标识一次工作；`user_tag` 仍由调用方解释，不能替代处理身份。FIT 与 REGION 的目标不同，但必须具有相同文件／选中图像／处理语义才允许组合；不能把整个 target 混入共享处理身份后导致所有细节都不匹配。具体输出 canvas／rect 在独立缓存键中处理。

默认值与显式相同值、别名格式、plane 顺序和浮点参数先规范化再比较；拒绝不合法 NaN／无穷配置。身份用实例内不透明非零值或规范描述的严格等价关系，不依赖未检查碰撞的短 hash。支持保留 INFO 锚点及必要内部身份记录；参数改变时创建新身份，不改写已借出对象。

## 6. 元数据探测与内容选择

新增有界异步 probe／catalog 操作，复用全局 request ID、控制线程与事件队列。探测请求不交付像素；返回受限数量／字节的条目和可继续枚举的位置，成功／失败／取消仍仅一个终结。不能 submit 时同步扫描整个容器，也不能为枚举悄悄完整解码所有条目。

条目至少包括稳定且版本锚定的 item 标识、角色、可知尺寸／位深／通道信息和实际能力。selector 可明确要求主图、指定 item／page／part／channel 映射或相机完整显影；ICO 自动尺寸策略要回报选中的条目。未找到指定条目不能回退到另一个内容却报告成功；自动选择只在显式允许时执行并回报结果。

probe 不提供文件快照。显示请求携带期望源版本／锚点来验证 probe→decode 之间文件未变；变更返回 SOURCE_CHANGED／重新探测，不能把老 item 索引用于新文件。HEIF item、TIFF page、ICO representation、EXR part／channel、动画 frame 属于不同选择维度，不能统一假设为一个“第 N 张”。

EXR／JPEG 2000 等非零原点、采样网格信息通过有符号源 window／origin 元数据表示；显示 canvas 仍归一为左上原点的现有无符号尺寸。EXR 区分 data window 和 display window，明确填充、裁切与像素宽高比；首轮对不能保持显示比例的非方形采样明确拒绝或声明归一策略，不默默拉伸。相机内嵌预览与完整显影回报不同角色和真实尺寸；CUR hotspot 是元数据，不是自动应用的图像偏移。

源／颜色／条目 metadata 有上限并计入 CPU 预算。getter 的借用视图随拥有者有效，需长期使用必须保留拥有者或复制；不能把第三方 decoder 内部指针直接交给 UI。

## 7. ROI、tile、相位和边缘

沿用 target 的未旋转输出 canvas／rect；不能改变已有 REGION 为“源像素裁剪”的含义。core 根据缩放关系求需要的源窗口，再执行色度对齐、滤波／去马赛克 halo、边界扩展和最终裁切。

- YUV 色度采样位置在源坐标下保持一致。4:2:0／4:2:2 的奇数起点、不同 stride 和 UV／VU 排列都要覆盖；CPU RGBA 输出可以支持任意合法逻辑 ROI，不要求调用方懂色度对齐。
- Bayer 的 CFA 相位来自完整源坐标；halo 半径由算法／质量模式确定，相邻 tile 不能在各自边缘重新插值出不同颜色。黑电平／白平衡／tone-map 的参数按整次处理固定，禁止每 tile 自动曝光造成接缝。
- 缩放滤波也需要一致的源采样和边界。全局统计型显示映射若需要预扫描，必须声明成本、限制与固定结果，不能为首块偷偷冻结错误统计后又混用版本。
- 原生 plane 输出后续另定义 chroma crop／相位及 shader 采样映射；一次 tile 的完整 plane 集原子交付，不能先交 Y 后改 UV。动画全帧原子切换同样覆盖全部 plane。
- REGION 可实现与高效随机访问分开声明。仅能全图解码／顺序扫描的输入，要报告成本和预算失败，不能为了兑现任意 ROI 而无限分配全源缓冲。

## 8. 预算、背压与取消

所有输出／队列／工作区准入从实际 layout 推导，不再固定每像素 4 字节。配置创建可只校验基线布局；新增高精度请求受理／计划阶段须保证至少一个实际 payload 能进入当前队列：缩小 tile 需遵守已声明格网，不允许在发布中途改格网；否则明确返回预算／目标错误，不能进入永不可能满足的等待。

计量分为拥有的实际分配、逻辑队列 payload 与借出引用，不能把三者直接相加当 RSS。共享 plane／父缓冲的实际分配只记一次；同一对象通过多个事件排队时要明确定义逻辑额度，不能靠重复指针绕过预算。保留像素的真实父分配比 tile 大时，也按真实拥有量计 CPU 预算。

完整工作集包括压缩输入／文件映射、raw plane、解包、halo、高精度中间图、色彩配置／转换、动画参考与合成、输出及事件、第三方保留量。支持 allocator hook／上限的后端优先使用，无法完整控制的库记录保守 allowance 和外部分配估计；不能把核心账本宣传为进程 RSS 硬上限。限制第三方线程，防止内部并行无界放大工作集。

源文件大小限制、解析 / metadata 上限、源尺寸上限、输出分档阈值和实际工作集准入分别定义。现有某些编码路径的 64MiB 文件限制不能无条件套到多 GiB 的合法 raw / 大图，也不能以取消文件上限代替有界解析和内存保护；`large_pixel_bytes` 的分档含义与安全硬上限不得继续混用。新格式的限制按真实解码路径记录和验收。

预算不足区分可等待的外部借用与自身永远不可能释放的固定需求；沿用当前避免单 worker 自锁的规则。探测 metadata、raw 描述深拷贝、profile、trace 均计预算，不为错误消息分配无上限文本。

读取行／块、解包组、去马赛克行／块、转换及队列等待都设协作取消点；单次无法打断的第三方调用明确记录限制。保留终结竞争和三阶段关闭规则，不承诺强杀线程或固定取消时限。新增 plane／metadata retain 对象也须纳入 destroy 的外部引用判断。

## 9. 能力与错误模型

| 层次 | 回答的问题 |
| --- | --- |
| 构建／后端能力 | 是否构建该解码器、支持哪些输入变体、静态／动画／probe／精度／颜色处理上限 |
| 具体源能力 | 此文件实际 codec／profile／位深／layout／页面／序列及可用元数据 |
| 请求组合能力 | 此 selection＋processing＋target＋output 能否在当前实现与预算内完成 |
| renderer 设备能力 | 可上传格式／plane／shader 路径、纹理边长、预算与输出颜色链路 |

`available` 继续表示实际实现可用，不由枚举存在自动变成 true。WebP 静态和动画分开报；APNG 可以作为 PNG 家族的动画 variant，避免扩展名不变时错误路由。HEIF／AVIF 的容器／编码家族与实际 codec 分开描述。原始输入种类与 RGB／YUV／Bayer 的具体存储像素格式也不是同一个枚举。

保留 REGION 接受能力的现有含义，另加访问路径元数据（顺序扫描、整图重建、原生区域／瓦片、缩放级别等）；不能把慢扫描升级为“高效随机访问”。profile／高位深读取与最终 HDR 输出分别声明。

补充错误 detail 的稳定类别与阶段：参数缺失／非法 raw 布局、有效但未实现的 packing／像素格式、选择条目不存在、颜色转换不支持、目标输出不支持、metadata／尺寸／工作集限制。具体描述名与编号待 ABI 冻结，不要求为每格式创建一个顶层错误码。

同步拒绝只覆盖无需 I/O 可判断的描述问题并清空输出 ID；文件截断、选择不存在、实际 profile 不支持等在 worker 异步返回。错误带字段／plane／处理阶段及适用的 required／limit，但不要求 UI 解析英文 diagnostic。无效输入与合法但未实现严格区分；编码文件的元数据错误不能用“回退 raw”掩盖。

## 10. 动画与多帧

复用既有会话 ID、epoch、一次一个 NEXT／RESET、只交付完整可显示帧、UI 播放时钟与循环规则。内部会话 decoder 改为带有界资源管理与清理方法的 codec-neutral adapter；这属于后续实现，不增加动态插件 ABI。

APNG 的默认静态图与真正 frame 0、blend／disposal；WebP 的完整画布／透明／循环；AVIF／JXL 的时间基、参考帧和显示帧关系由各适配器归一。隐藏的解码参考更新不作为 UI 帧发送，也不计入“呈现帧数”；解码工作量可在诊断另记。不能把容器多张独立图片自动当作动画。

播放用 ns 统一时钟，保留已知／未知／零时长标记；源有理数 duration／timescale 作为可选精确元数据提供，并规定受检换算与统一舍入。累计 PTS 基于精确累计时间转换，避免逐帧舍入无限累积；溢出／未知不回绕，清对应 KNOWN 位。循环归一保持总轮数语义，逐格式核对，不能假定全部沿用 GIF 规则。

会话冻结 selection／raw layout／处理／输出策略；改变白平衡、颜色或格式要创建新会话，不能让 RESET 静默改配方。raw 多帧扩展若实施，也走受控普通尺寸会话，不对文件尾部自动推断，无随机 seek 或超大动图承诺。

## 11. 逐格式兼容落地路线

以下均为 **待开发**。后端仅列评估方向，不表示已经选型、安装或锁定版本；版本、构建选项、依赖与发行要求实施时再评审。

| 计划项 | 先行支持路径 | 必要接口补充／后端方向 |
| --- | --- | --- |
| AVIF | 主图 SDR 预览，再高精度／普通动画 | 颜色／位深／时间／角色；评估 libavif，实际 AV1 后端能力单列 |
| JPEG XL | 静态显示，再渐进／高精度／动画 | 源／输出颜色、细化、时间、实际显示帧；评估 libjxl |
| HEIF／HEIC | 明确编码的主图，再集合／序列 | probe／item selector、方向／裁切／颜色；评估 libheif 和实际 codec 支持 |
| APNG | 通用会话上的完整普通动画 | APNG parser／动画合成 adapter 与 PNG 帧解码，不能只给 libpng 开一个能力位 |
| 动画 WebP | 沿用 GIF 的会话／呈现契约 | WebP 动画 adapter；评估现有 libwebp 的动画／demux 组件，当前静态链接能力不能直接代替 |
| ICO／CUR | 默认最佳尺寸，再显式条目 | 有界条目解析、PNG／DIB、alpha／mask、hotspot；复用经过验证的静态解码子集 |
| QOI | RGB／RGBA→RGBA8 | 评估参考解码器／有界适配；不强制改变像素输出 ABI |
| JPEG 2000 | JP2／J2K 明确子集→SDR | 位深／通道／原点、颜色、缩放／区域成本；评估 OpenJPEG |
| OpenEXR | 单选 part／RGB(A) 通道→SDR，再浮点输出 | probe／part／channel、window／origin、NaN／Inf 显示政策、颜色映射；评估 OpenEXR；deep／复杂特性明确另项 |
| 相机 RAW／DNG | 内嵌预览与完整显影独立 | camera／variant 支持、active area／CFA／标定、预览角色、显影配方；评估 LibRaw，不能承诺所有机型 |
| 原始 RGB／灰度 | 显式布局→CPU 规范化 RGBA8 | raw descriptor、位深／字节序／alpha／stride／offset；高精度另期 |
| 原始 YUV | 显式布局→CPU 转 RGB，再 GPU plane 路径 | raw descriptor、matrix／range／siting／transfer、ROI；可评估 libyuv 的具体支持子集 |
| 原始 Bayer | 显式 unpack／去马赛克→SDR | CFA／packing／电平／白平衡／标定、halo／ROI、算法可复现；与相机容器解码分层 |

实施顺序建议：

1. 校准当前 API／Runtime／query 与 REGION 工作区实现的文档差异，保留现有契约和测试。
2. 可先用 ABI 2 增加不需要新请求语义的 RGBA8 adapter／普通动画；每格式独立验收，只声明实际 SDR 子集。
3. 冻结下一主 ABI，落地 raw 描述、selection／probe、处理身份、颜色／输出政策和有界诊断；再接入 RGB／YUV／Bayer 及需要选择／显影的格式。
4. 在 CPU 预览准确且有基准后，再联合 renderer 开发高精度／HDR／原生 plane 与 GPU 转换；逐格式开启真实能力。

第二步与第三步的依赖要按具体子能力判断，不要求先实现全部新格式才升级 ABI，也不能以“已能预览”宣布该格式全部完工。没有锁定排期。

## 12. 性能诊断接口补充

为每个接受的请求／动画帧可选提供有界诊断摘要，成功、错误、取消都有；不只在 COMPLETE 上提供总耗时。建议摘要随终结事件拥有／锚定，或通过明确寿命的只读 accessor 取得；不能要求请求槽释放后再按旧 ID 查询仍存在的统计。

最低字段为 request_id、user_tag、源／选择／处理身份、frame key、实际后端和 CPU／GPU 路径、源／实际解码／输出尺寸、读取字节、有效字段位及各阶段 wall ns／可用 CPU ns。阶段包含排队、I/O、压缩解码、unpack、色度重建／YUV 转换、去马赛克、颜色／显示映射、resize、分配、预算／队列等待和终结。统计递归包含关系明确：重叠阶段不可直接相加，外部 codec 无法拆分时报告合并区间与 unknown，不填假零。

core 完成只表示 CPU 数据交付契约完成，不表示上传、GPU 执行或实际可见。viewer／renderer 用 request／processing／frame key 关联上传 CPU、GPU query、present 和外部可见时间；GPU 时钟与 CPU 时钟未校准时只报告各自区间，不拿时间戳直接相减。

摘要和 trace 有独立受预算约束的容量，关闭时开销可控；溢出报告丢弃计数，不阻塞关键控制事件。不要为每个 stage 新增必达控制事件而破坏现有 INFO／terminal 预留，也不要把日志输出作为唯一精确计时机制。CPU／RSS／GPU 利用率仍由外部采集，与 core 账本和摘要分别报告。

## 13. 实施前必须补齐的验收

- [ ] ABI：旧版本／错误 size 明确拒绝；所有新 view、plane／metadata 借用、初始化和 event union 的 C／C++ 布局验证。
- [ ] raw：同字节按 NV12／NV21、YUYV／UYVY、大小端及不同 packing 解读，参数深拷贝后调用方修改／释放不影响在途请求。
- [ ] 边界：不足 stride、最后一行无尾 padding、非零 offset、plane 重叠、乘加溢出、超 4GiB 偏移与截断，不能越界或阻塞 UI。
- [ ] 颜色：601／709、limited／full、色度位置、高位深／ICC／未知配置、alpha 语义和 HDR→SDR／原生 HDR 分别验收。
- [ ] Bayer：四种 CFA、四种 ROI 起点奇偶、各 packing、黑白电平、白平衡、halo／边界，整图与拼接 ROI 的结果在算法约定误差内一致。
- [ ] 选择：ICO 条目、HEIF 主图／item、TIFF page、EXR part／channel、RAW 预览／完整显影；probe 后文件变化不能复用老 selector。
- [ ] 身份：同文件改变 layout／矩阵／显影／输出精度后不得混合 FIT／REGION／缓存；相同默认和显式参数得到规范等价结果。
- [ ] 预算：RGBA8／16／float／多平面均检查最小可交付块，metadata／外部库／trace 不逃逸，低预算不得形成自等待。
- [ ] 动画：各格式 frame 0、合成／参考帧、精确时间归一与循环；取消／RESET／close、全部 plane 的完整帧呈现。
- [ ] 生命周期：释放 event／INFO 后 retained pixels 的所有 plane／颜色信息有效，shutdown 后可归还，最后外部引用前 destroy 保持 BUSY。
- [ ] 性能：三个图片类别的打开／处理／渲染／CPU／内存／GPU 全覆盖，取消和失败也有有效诊断；无压缩 raw 的解码项为不适用。

## 14. 核对来源

以下是接口和格式语义的核对依据，不代表已经选定依赖或完成适配：

- [libavif 公共头文件](https://github.com/AOMediaCodec/libavif/blob/main/include/avif/avif.h)：源 plane／位深、颜色元数据及输出转换。
- [libjxl metadata API](https://libjxl.readthedocs.io/en/latest/api_metadata.html)：源信息、extra channels 和动画时间／帧语义。
- [libheif context API](https://github.com/strukturag/libheif/blob/master/libheif/api/libheif/heif_context.h)：主图和顶层图像条目。
- [OpenEXR 读取／写入文档](https://openexr.com/en/latest/ReadingAndWritingImageFiles.html)：通道、data／display window 和存储形态。
- [LibRaw API](https://www.libraw.org/docs/API-CXX.html)：unpack、处理与图像生成分层；相机元数据不能由裸像素描述替代。
- [V4L2 YUV 布局](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-yuv-planar.html) 与 [Bayer 布局](https://docs.kernel.org/userspace-api/media/v4l/pixfmt-srggb10p.html)：plane／packing／色度和 CFA 定义。
