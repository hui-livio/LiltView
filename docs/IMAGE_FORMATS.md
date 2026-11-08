# 图片格式与解码实现

当前支持 JPEG、PNG、基础静态 SVG、静态 WebP、BMP、GIF 自动播放和 TIFF 首页的后台打开与 FIT 显示。
UI 继续使用现有命令行、菜单 / Ctrl+O 和拖放入口；本轮不增加产品交互。
本表是实际兼容范围；[总体设计](DESIGN-v0.1.md)中的扩展格式和完整 SVG 验收仍是后续规划。

| 格式 | 后端 | 已实现范围 | 限制 |
| --- | --- | --- | --- |
| JPEG / JPG | libjpeg-turbo 3.1.2 | 8 位 RGB / 灰度，baseline / progressive 最终结果，EXIF 1..8，缩放解码 | CMYK / YCCK、12 位及扫描预览未实现 |
| PNG | libpng 1.6.58 + zlib 1.3.2 | RGB / RGBA / 灰度 / 灰度 alpha / 索引色，合法 1 / 2 / 4 / 8 / 16 位组合，tRNS，Adam7 | 16 位缩至 8 位；APNG 明确拒绝；不应用 ICC、gamma 或 PNG EXIF |
| WebP | libwebp 1.6.0 decoder | 静态有损 / 无损、alpha、解码器缩放 FIT | 动画明确拒绝；不应用 ICC / EXIF 方向 |
| BMP | 项目 C23 解码器 | 1 / 4 / 8 位调色板，16 / 24 / 32 位真彩，行对齐、上下行序，16 / 32 位 bitfields，RLE4 / RLE8 | 不支持内嵌 JPEG / PNG、OS/2 扩展压缩和色彩配置转换；RLE 不允许 top-down |
| SVG | PlutoSVG 0.0.8 + 固定 PlutoVG 1.3.3 | 路径、基本几何、组、变换、fill / stroke、透明度、线性 / 径向渐变、viewBox，按 FIT 栅格化并允许放大 | 下述受限静态 profile，不等同于浏览器 SVG |
| GIF | giflib 6.1.3 | GIF87a / GIF89a 动画会话，透明、交错、局部调色板、disposal 0..3、循环次数与源帧时长 | 普通尺寸顺序播放；静态 submit 仍可仅读首帧；无 seek / 超大动图 |
| TIFF / TIF | LibTIFF 4.7.2 | Classic / BigTIFF 普通尺寸首页；大小端、strip / tile、常见压缩、方向、灰度 / RGB / 调色板及显式 alpha | 不支持页面切换、浮点 / CMYK；调色板 16 位等不受支持组合明确拒绝，仍受内存限制 |

BMP 接受 12 / 40 / 52 / 56 / 108 / 124 字节 DIB 头，位域掩码必须连续、不重叠且在位深范围内。
BI_RGB 32 位的第四字节视为保留位，统一不透明；只有显式 alpha mask 才使用 alpha。
RLE 的跳过区域使用调色板第零项填充，检查 run、delta、绝对块、padding 和结束标记。

GIF 已通过 ABI 2 会话及 UI 自动播放；TIFF / BigTIFF 显示普通尺寸首页。动画 WebP / APNG、多页切换、AVIF、HEIF / HEIC、静态超大图、REGION 和预读仍未实现。[GIF 动画接口与实现](CORE_ANIMATION-v0.2.md)。
不支持 CAD、PSD / PSB 等专用设计源文件。
后续开发范围分别为普通尺寸动图播放与静态超大图显示；超大动图明确不纳入范围。动画 FIT 缩小不能绕过源逻辑画布及实际工作集的限制，详见[动画范围约定](CORE_ANIMATION-v0.2.md#11-已确定的开发范围)。

## 共用加载链路

```text
UI submit → 单 worker 打开常规文件并取得版本
          → 有界文件头按内容探测 → 选择 codec
          → 尺寸 / 格式检查 → 工作集预算预约
          → IMAGE_INFO → 解码或栅格化 → RGBA8 FIT 图
          → 销毁 codec / 临时数据 → 验证文件及路径版本
          → 有界队列交付最终 tile → COMPLETE
UI poll → 上传 renderer → release event / pixels
```

文件扩展名和大小写不决定 decoder；同一个已打开文件用于探测、解码及最终版本核验。
修改文件后不发布旧版本像素；INFO 可以早于解码完成。已经借出的事件 / 像素寿命、取消竞争及关闭规则沿用 [Core Runtime](CORE_RUNTIME-v0.1.md)。
损坏输入异步返回 ERROR，worker 可继续接收下一张；单次请求至多一个终结事件。
代码分为 `image.c` 共用准入 / FIT / tile / alpha 缩放，以及每格式独立文件；公共 C API 不暴露第三方依赖类型；动画会话接入后布局已升级为 ABI 2。

query 对七类格式均返回静态 available；JPEG / WebP 报 SCALED_DECODE，SVG 报 VECTOR_RASTERIZE，GIF 另报 ANIMATION，PNG / BMP / TIFF 无额外能力位。
不报告 INCREMENTAL / REFINEMENT / REGION_DECODE。`load_mode` 仍是大小阈值分类，不代表已实现解码中逐步显示。
静态请求及每个动画帧请求都先完成解码与版本核验，再交付最终 tile；WebP 的增量输入仅用于在输入块之间检查取消。

## 像素、内存与取消

所有输出为 top-left、紧密 RGBA8、straight alpha；没有完成 ICC / HDR 色彩管理，颜色语义为 UNKNOWN。
PNG / BMP 的 FIT 使用 alpha 加权双线性，SVG 的原生 premultiplied ARGB 在交付前转换为 straight RGBA。
JPEG 保留缩放解码加两行 FIT 缓冲的路径。

| 项目 | 当前边界 |
| --- | --- |
| 位图源文件 | 最大 64 MiB；不代表此范围全部能在当前预算内显示 |
| 位图原始 RGBA | 不超过 `min(large_pixel_bytes, 256 MiB)`，FIT 很小也不绕过源尺寸限制 |
| SVG | 最大 1 MiB、8192 个元素、128 层嵌套；输出 RGBA 同样受上述像素限制 |
| UI 默认预算 | CPU 384 MiB、队列 4 MiB、renderer 图片 128 MiB |
| 最终交付 | 提前预约完整 FIT 图及其全部 tile / 事件潜在峰值，队列按字节背压 |

PNG 为 Adam7 保留完整原始 RGBA，再生成 FIT；BMP 保留完整文件与原始 RGBA，再生成 FIT。
WebP 保留完整压缩输入，使用外部 FIT 输出缓冲，并保守预约内部解码工作集。
SVG 保留源文本与文档树，直接绘制到 FIT 缓冲。GIF 动画保留 decoder、完整逻辑画布和 disposal 备份，最多另外生成一帧 FIT 候选；驻留总子预算默认 min(CPU/2, 128 MiB)，UI 两张纹理均受现有 GPU 预算约束。
因此低于硬尺寸限制的图片仍可能因实际 CPU / GPU 预算返回 MEMORY_BUDGET。
这轮不是超大图的低内存解码实现。

可追踪的输入、原始图、FIT、tile 和事件分配进入 core 账本；外部分配按保守 allowance 提前预约。
第三方内部 allocator、C runtime 和线程栈未完全纳入追踪，预算不承诺进程 RSS 硬上限。
在文件读块、PNG 行 / pass、BMP 行 / RLE run、缩放行和 tile 队列等待处检查取消。
PlutoSVG 的一次 parse / render 和 WebP 的单次解码调用无法被中途打断；取消会在返回后阻止像素发布，不能据此承诺固定关闭延迟。

## SVG 静态 profile

允许的元素为 svg、g、defs、path、rect、circle、ellipse、line、polyline、polygon、linearGradient、radialGradient、stop、title、desc。
支持简单 inline style 和 presentation attributes，受 PlutoSVG 实际能力约束。
字体 / text、CSS stylesheet / class、clipPath、mask、filter、use / symbol、image（含 data URI）、foreignObject、脚本、动画、DTD / 实体等明确返回 FORMAT_UNSUPPORTED。
当前不访问外部文件或网络，也不扫描系统字体。没有自然尺寸时使用库的 viewBox / 默认 300×150 规则；无有效尺寸或非有限值则拒绝。
profile 是保守兼容子集，不是通用 XML 验证器；属性和路径语法仍由 PlutoSVG 解析，不能宣称完整 SVG2 一致性。

## 构建与验证

需要编译的依赖均固定为 Git submodule；PlutoVG 由 PlutoSVG 的嵌套 gitlink 固定。
执行 `git submodule update --init --recursive` 后由 CMake 使用本地源码静态构建，不自动下载，不选择系统 codec 库。
主程序仅链接 WebP decoder，测试编码器只用于生成测试输入；上游工具、示例、测试程序不进入应用。
安装包包含 libpng、zlib、libwebp 的 COPYING / PATENTS、PlutoSVG / PlutoVG 许可证、bundled stb 署名及 FreeType FTL。

格式测试覆盖内容探测、PNG 位深 / Adam7 / tRNS、WebP 有损 / 无损 / alpha、BMP 位深 / 掩码 / RLE / 行序、SVG 渐变 / FIT / alpha，以及损坏输入、动画拒绝、预算、修改源文件和取消。
`tests/fixtures/` 保存原创小样本，desktop.formats 用真实 OpenGL 回读截图检查六类格式的红色内容；desktop.animation 验证真实 OpenGL 红 / 蓝帧切换，desktop.animation_app 验证应用进入下一轮。
最新构建验证记录见 [目录与构建](PROJECT_STRUCTURE.md)。
