# 图片格式兼容表

更新：2026-11-11，按当前源码核对。本文只列实际兼容子集，新增格式见[开发计划](DEVELOPMENT_PLAN.md)；后台加载和渲染链路见[架构](ARCHITECTURE.md)与[渲染说明](RENDERING.md)。

## 1. 已支持格式

| 格式 / 后端 | 已实现范围 | 主要限制 |
| --- | --- | --- |
| JPEG / libjpeg-turbo | 8 位 RGB / 灰度，baseline / progressive 最终结果，EXIF 1..8，缩放解码 | 无 CMYK / YCCK、12 位、扫描预览或 ICC 转换 |
| PNG / libpng + zlib | RGB / RGBA / 灰度 / 灰度 alpha / 索引色，合法 1 / 2 / 4 / 8 / 16 位组合，tRNS、Adam7 | 16 位转 8 位；APNG 拒绝；不应用 ICC、gamma、PNG EXIF |
| WebP / libwebp decoder | 静态有损 / 无损、alpha、FIT 缩放 | 动画拒绝；不应用 ICC / EXIF 方向 |
| BMP / 项目 C23 decoder | 1 / 4 / 8 位调色板、16 / 24 / 32 位真彩、上下行序、bitfields、RLE4 / RLE8 | 无内嵌 JPEG / PNG、OS/2 扩展压缩或色彩配置转换；RLE 不支持 top-down / REGION |
| SVG / PlutoSVG + PlutoVG | 基础静态 profile：路径 / 几何 / 渐变 / viewBox；Core 可按 FIT / 自然尺寸栅格化 | **复杂 SVG 不支持**；应用缩放复用已栅格化像素，不保证任意放大保持矢量清晰；具体拒绝项见第 4 节 |
| GIF / giflib | GIF87a / GIF89a，普通尺寸自动播放，透明 / 交错 / 局部调色板、disposal 0..3、源时长和循环；应用已接入邻居会话 / 首帧预热 | 顺序 NEXT / RESET，无 seek / 超大动图；静态 API 可仅读首帧；UserInputFlag 拒绝 |
| TIFF / LibTIFF | Classic / BigTIFF 首页，大小端、strip / tile、常见压缩、方向、灰度 / RGB / 调色板及显式 alpha | 无页面切换、浮点 / CMYK；调色板 16 位等未支持组合明确拒绝 |

具体依赖提交与版本以 [dependencies.json](../third_party/dependencies.json) 及 gitlink 为准，不在多份文档重复维护版本号。窗口入口支持命令行、菜单 / Ctrl+O 和单文件拖放，随后后台枚举同目录的支持后缀供导航；解码器仍按文件内容选择。

BMP 接受 12 / 40 / 52 / 56 / 108 / 124 字节 DIB 头；位域掩码须连续、不重叠且在位深范围内。BI_RGB 32 位第四字节视为保留位，只有显式 alpha mask 才使用 alpha。RLE 跳过区域填调色板第零项，并检查 run、delta、绝对块、padding 和结束标记。

## 2. 静态、大图与动画能力

| 路径 | 当前能力 | 成本 / 限制 |
| --- | --- | --- |
| JPEG、非交错 PNG | NATIVE 原始行带、FIT、REGION | 顺序扫描；REGION 不等于原生随机访问 |
| Progressive JPEG | 同一最终输出接口 | 保守预约完整系数工作集；没有扫描细化，不保证低内存首行带 |
| Adam7 PNG | 完整合并后输出 | 需要原图合并缓冲，超预算可失败；没有逐 pass 细化 |
| 未压缩 BMP / bitfields | NATIVE 行带、FIT、REGION，按需 seek 行 | 极宽单行仍须满足工作集预算 |
| TIFF 首页 | 有界局部块解码、NATIVE 行带、FIT、REGION | 源边长不超过 INT32_MAX，块与 codec 内存有界；core 缩小采样当前为最近邻 |
| WebP、SVG、RLE BMP、静态 GIF | 各自有界完整解码 / 栅格化输出，可分块交付 | 不因此获得静态 REGION 或任意超大源能力 |
| GIF 动画 | 会话持续合成，整帧交付与 UI 双纹理自动播放 | 源画布、disposal 备份和输出均准入；缩小 FIT 不能绕过源预算 |

静态 app 使用单次 NATIVE，随后缩放 / 平移从当前场景的原始 tile 与预生成 LOD 绘制或恢复，不再重新请求 REGION。core FIT / REGION 继续可用；流式行带是最终像素提前交付，**不是解码扫描中的多轮 refinement**。

GIF 邻居由应用按内容识别，预热有界 FIT 会话 / 首帧，后台不上传 GPU。切换时先用 NEXT 重验源并取得真实后续帧或 END，再安装预热首帧一次，后续帧 / 循环沿用源时长和总轮数；版本、FIT 尺寸或资源不符则普通重开。它不等于 Core 完整动画内容缓存；历史验证记录见[开发计划](DEVELOPMENT_PLAN.md#7-历史功能开发与验证记录2026-11-10)。

构建查询返回格式能力上限；具体文件 INFO 收窄，例如 RLE BMP 不报告 REGION。JPEG / WebP 可报告 SCALED_DECODE，SVG 报 VECTOR_RASTERIZE，GIF 报 ANIMATION；JPEG / PNG / BMP / TIFF 具有区域能力。当前不报告 INCREMENTAL / REFINEMENT，load_mode 只是大小分档，不能当作已实现扫描细化的证据。

## 3. 输出、资源与失败

- 像素为左上原点、RGBA8、straight alpha；stride 可大于 width×4。方向交给 viewer 应用，颜色语义为 UNKNOWN，没有完整 ICC / HDR 色彩管理。
- PNG / BMP 的 core FIT 使用 alpha 加权双线性；SVG 原生预乘 ARGB 转为 straight RGBA。app LOD / atlas 是另一层显示处理，不等同于 codec FIT。
- JPEG、PNG、未压缩 BMP、TIFF 不套统一 64 MiB 文件 / 完整 RGBA 上限；仍按实际工作集、源元数据和输出准入。流式 NATIVE 只保留活动行带，逻辑 canvas 不等于整图分配。
- FIT / REGION 单次输出不超过 `min(large_pixel_bytes, 256 MiB)`；WebP、静态 GIF、RLE BMP 等保留 64 MiB 文件与既有原图 RGBA 限制。SVG 最多 1 MiB、8192 元素、128 层，并受输出预算限制。
- 内容探测、解码及版本核验使用同一个已打开文件；流式块发布前检查版本，读取结束再检查。检测到源变化即报 SOURCE_CHANGED，不能把部分输出与新版本拼接；不承诺文件快照。
- 最终像素以引用移交或共享 backing 视图发布，不为排队深拷贝。第三方工作集按保守 allowance 准入，不保证整个进程 RSS 硬上限；所有权与关闭见 [Core 契约](CORE_CONTRACTS.md)。
- 七类静态结果均可参加可选 Core 内存内容缓存 / PREFETCH，按完整目标和源版本复用，过大结果不无限常驻；GIF 此处仅指静态首帧，应用会话预热是独立路径。Core 磁盘内容缓存和通用完整动画缓存仍未实现。
- 文件读取、行 / pass / run、转换和队列等待有取消点；PlutoSVG 单次 parse / render、WebP 单次 decode 不能中途强杀。取消后阻止新像素发布，不保证固定关闭延迟。

## 4. SVG 静态 profile

**产品定位：仅支持基础静态 SVG，复杂 SVG 当前不支持，也不承诺后续交付日期。** “复杂”按所用特性而非文件大小判断：即使文件很小，只要依赖下列不支持特性，也不属于当前支持范围。浏览器能显示不代表 LiltView 能显示；文件扩展名和 `VECTOR_RASTERIZE` 能力位不代表完整 SVG 兼容。

允许：`svg`、`g`、`defs`、`path`、`rect`、`circle`、`ellipse`、`line`、`polyline`、`polygon`、`linearGradient`、`radialGradient`、`stop`、`title`、`desc`，以及库支持的变换、fill / stroke、透明度和简单 inline style / presentation attributes。

明确拒绝：text / 字体、CSS stylesheet / class、clipPath、mask、filter、use / symbol、image（含 data URI）、foreignObject、脚本、动画、DTD / 实体。不读取外部文件或网络，不扫描系统字体。没有自然尺寸时按 viewBox / 默认 300×150 规则；无效尺寸拒绝。此 profile 不代表完整 SVG2 / XML 一致性。

| 场景 | 当前状态 |
| --- | --- |
| 基础路径、几何、渐变、变换、透明度和 viewBox | 仅在上述 profile 及资源限额内支持 |
| 文字 / 字体、CSS stylesheet / class、clipPath、mask、filter、use / symbol、image / data URI | 不支持，profile 检查显式拒绝，不以缺失重要内容的结果代替成功 |
| 脚本、动画、foreignObject / HTML、DTD / 实体、外部资源 | 不支持；不执行脚本、不联网、不读取 SVG 引用的外部文件 |
| 任意缩放后的矢量重新栅格化 | 未实现；当前应用按自然尺寸栅格化后复用已有像素 / LOD |

当前 SVG 输入上限为 1 MiB、元素上限 8192、嵌套深度上限 128，另受像素尺寸和工作集预算限制。超出 profile 的已识别特性按 `LV_ERROR_FORMAT_UNSUPPORTED` 拒绝；达到输入 / 元素 / 嵌套限额按 `LV_ERROR_DIMENSION_LIMIT` 拒绝，损坏输入和预算失败分别处理。准入检查是保守 profile 检查，不是完整 SVG/XML 语义验证，不能保证发现所有不支持的属性或语法。

需要查看复杂 SVG 时，请在能正确显示它的工具中按所需分辨率导出 PNG，再用 LiltView 打开。仅将文字转路径不能保证兼容，剩余的滤镜、遮罩、样式和引用等仍可能超出 profile。

轻量化约束及重新评估条件见[开发决策](DEVELOPMENT_PLAN.md#8-复杂-svg-范围与技术决策)。这里的“不支持”描述当前产品能力，不等于断言不存在可处理复杂 SVG 的 C/C++ 库。

## 5. 待实现与验收入口

AVIF、JPEG XL、HEIF / HEIC、APNG、动画 WebP、ICO / CUR、QOI、JPEG 2000、OpenEXR、相机 RAW / DNG、TIFF 多页、原始 RGB / 灰度 / YUV / Bayer 全部待实现；高位深读取、SDR 映射、原生 HDR 与 GPU 多平面分别验收。计划不改变本表已支持范围。

合法 / 损坏输入、位深、方向、预算、源变更和取消测试见 [`tests/core/`](../tests/core/)，真实像素回读见 [`tests/integration/`](../tests/integration/)；输入样本与性能方法见[测试计划](../benchmarks/TEST_PLAN.md)。本文状态来自源码核对，没有把历史通过数量当作本次运行结果。
