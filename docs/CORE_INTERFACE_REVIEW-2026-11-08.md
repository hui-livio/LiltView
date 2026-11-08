# Core 接口兼容性 Review（2026-11-08）

## 范围与结论

基线提交：`1d7ae5def6834cbcf2847ec6a6ebe4400fd9f1f6`。审阅对象为该提交上的 **当前工作区** 公共头文件、请求／事件／动画／预算实现、Core API／Runtime／Animation 契约，以及 renderer 上传接口。工作区已有尚未提交的 REGION／超大图开发，本次不修改或认定这些实现已经验收。

公共头文件 `core/include/liltview/core.h` 的 Git blob 为 `447157a7f88538090f6817d2ceaca42105e1a07c`。renderer 头文件初读为 `12f46debb8fb4c6ceb52b5c5e1c8c2dc6c855248`，末次复核为 `c47de4f4299e83ddd204f63c53a1f3328d253821`：并行开发增加了 linear／nearest 放大采样设置，仍是 RGBA8 单平面上传，相关结论不变。实现文件也在并行开发，因此用函数名和文件定位，公共头文件行号对应此次审阅。只做静态接口 review 和文档补充，没有修改代码、ABI 常量或构建依赖，没有运行或宣称新增格式测试通过。

**结论：保留现有异步请求、不可变事件／像素、有界交付和关闭模型；现有 ABI 2 足以承接部分新增格式的 RGBA8 静态／普通动画显示，但不足以完整表达原始 RGB／YUV／Bayer、子图选择、高精度颜色及原生多平面输出。** 建议按 [兼容扩展方案](CORE_FORMAT_EXTENSION_PLAN.md) 分阶段实施，需要改变结构布局时统一迁移到下一主 ABI，不在 ABI 2 上追加字段后继续声称二进制兼容。

下面的 P1／P2 表示对新增兼容目标的阻断程度，**不是宣称当前已支持格式存在同等严重的运行故障**。P1 为相应能力接入前必须补齐，P2 为完整验收、诊断与后续扩展需要补齐。

## Review findings

### R01 · P1 · 请求没有原始输入描述，宽高与布局无法传给 core

证据：[lv_request_desc](../core/include/liltview/core.h#L255) 只有路径和显示 target；[lv_validate_request](../core/src/api/engine.c) 检查现有 target／路径，内容探测位于 [image.c](../core/src/codecs/image.c)。`target.fit_bounds` 是输出显示尺寸，不能借用为源数据宽高。

后果：同一个 `.raw`／`.yuv` 文件无法明确指定 NV12、YUYV、字节序、plane stride／offset、Bayer CFA／packing；增加扩展名或格式枚举不能补上信息。建议引入显式编码文件／原始文件输入种类、原始存储描述，以及与存储分离的处理参数。关联方案 §2、§3。

### R02 · P1 · 源／输出颜色和高精度结果无法完整表达

证据：[lv_pixel_format／lv_color_space／lv_pixel_view](../core/include/liltview/core.h#L192) 只有 RGBA8、UNKNOWN／sRGB 和单个 data／stride；[lv_pixels_new](../core/src/engine/objects.c) 固定输出 RGBA8／UNKNOWN。现有 INFO 没有源位深、ICC／CICP、YUV range／matrix 或 Bayer 标定信息。

后果：AVIF／JXL／HEIC 的高位深／HDR、EXR 的浮点值、YUV 的矩阵／范围和 Bayer 的显影不能被准确声明；只改 format 常量会截断精度或把不明颜色当正确输出。建议区分源描述、处理配方、实际输出描述，先做声明明确的 SDR 预览，再增加高精度／原生平面输出。关联方案 §4。

### R03 · P1 · 文件版本不是像素解释版本，同文件多种参数可能被错误混用

证据：[lv_image_info.source_revision](../core/include/liltview/core.h#L272) 锚定文件版本；[lv_source_for_locked](../core/src/engine/objects.c) 只比较可观测文件身份。`user_tag` 明确由 core 原样回传、不参与解释。

后果：同一文件按 NV12／NV21、不同矩阵／白平衡或不同 EXR 通道生成的结果，会拥有相同文件版本。若以后仍仅用 source_revision 判断 FIT 与 REGION 可叠加，可能出现异色或错图；未来缓存也有同样风险。建议保留文件版本语义，另加被选图像／解释／处理身份，并定义独立的输出缓存键。关联方案 §5。

### R04 · P1 · 缺少可探测、可选择的子图／通道与结果角色

证据：[lv_request_desc](../core/include/liltview/core.h#L255)、[lv_image_info](../core/include/liltview/core.h#L272) 没有 page／item／part／channel selector，也没有枚举图像条目的操作或事件。

后果：ICO 多尺寸、HEIF 主图／集合、TIFF 多页、EXR part／channel、相机 RAW 内嵌预览与完整显影无法由调用方明确选择；不同来源的预览可能被当作原图。建议有界异步探测／枚举、明确选择器和实际选中项回报，不能把静态条目索引当动画帧索引。关联方案 §6。

### R05 · P1 · 原始 YUV／Bayer 的 ROI 不能直接复用 RGBA 矩形裁剪

证据：[lv_target](../core/include/liltview/core.h#L146) 定义未旋转输出画布，公共矩形没有色度相位或处理边界描述；`core/src/codecs/raster.c::filter_row`（审阅时的未提交工作区文件）的采样路径基于 RGBA8。

后果：直接以输出 rect 读取 4:2:0／4:2:2 会漏掉相邻色度样本，Bayer tile 独立去马赛克会产生接缝或 CFA 相位错误。建议继续让调用方请求逻辑显示 ROI，由 core 推导源采样窗口、对齐、halo 和 CFA 相位；native plane 输出另明确定义 plane crop。EXR 带非零／负原点的窗口也必须先归一坐标。关联方案 §7。

### R06 · P1 · 内存和队列准入固定按每像素 4 字节估算

证据：[config_valid](../core/src/api/engine.c) 和创建准入按 `tile_edge²×4`；[lv_pixels_new](../core/src/engine/objects.c)、[lv_frame_prepare](../core/src/codecs/image.c) 同样基于 RGBA8。

后果：若原样扩展为 RGBA16／浮点／多平面，可能低估工作区／输出体积，或让单块 payload 大于队列上限而永久等不到空间。Bayer halo、色彩转换中间图、第三方 allocator／线程也不能由最终 RGBA8 大小代替。建议按协商出的实际布局计算，接受像素块发布前验证队列可容纳，定义完整 CPU 工作集与外部分配边界。关联方案 §8。

### R07 · P1 · 结构精确匹配，没有 ABI 2 原位扩展空间

证据：[lv_core_create](../core/src/api/engine.c)、[lv_validate_request](../core/src/api/engine.c)、[动画 descriptor 校验](../core/src/api/animation.c) 要求精确大小；像素／事件 view 等结构没有独立版本前缀。[CORE_API §3](CORE_API-v0.1.md#3-abi-与初始化) 已明确不兼容变化必须升 ABI。

后果：在旧 request／event union 中加入 raw／color／selector 字段却保持 ABI 2，会与旧头文件、初始化器和 C／C++ 布局检查冲突。建议下一主 ABI 整体冻结、全部消费者重编译；仅增加常量或内部 adapter 不必自动视为结构 ABI 破坏。关联方案 §1、§11。

### R08 · P2 · 格式能力查询粒度不足以表达新的兼容子集

证据：[lv_format_support](../core/include/liltview/core.h#L460) 只有 available／capabilities；[lv_core_query_format](../core/src/api/engine.c) 限定现有格式区间，工作区已对部分格式报告 REGION。文件级 INFO 也只有相同的粗粒度位集。

后果：HEIF 容器识别不等于具备该文件的实际 codec；PNG 支持不等于 APNG 播放；REGION 接受不等于高效随机访问；源支持高位深不等于 renderer 支持 HDR。建议区分构建能力、特定输入实际能力、目标／输出组合能力和设备能力，报告区域访问成本类别，不承诺固定延迟。关联方案 §9。

### R09 · P2 · 公共动画模型可复用，内部实现仍绑定 GIF，时间精度也需说明

证据：[lv_animation_open_desc／frame_info](../core/include/liltview/core.h#L310) 的会话／帧身份和时间字段基本通用；[lv_animation_decode／cleanup](../core/src/engine/animation.c) 直接调用 GIF；[lv_animation.decoder](../core/src/internal.h) 是 GIF 专用类型。源时间当前为整数 ns。

后果：APNG／WebP／AVIF／JXL 不能只开能力位就播放。未来格式的默认图、隐含引用帧、合成、timescale 和循环语义必须适配为“完整可显示帧”。现有 ns 可用于播放，但不能精确保留所有有理数时间供元数据／基准分析。建议内部 codec-neutral adapter，沿用 NEXT／RESET、epoch、整帧原子呈现；有需要时增加源有理数时间和统一舍入策略。关联方案 §10。

### R10 · P2 · 单个完成耗时无法支撑本会话要求的瓶颈定位

证据：[lv_completion_info](../core/include/liltview/core.h#L303) 只有总 elapsed／读取字节；[finish_request](../core/src/engine/worker.c) 只给 COMPLETE 填该结构；[lv_core_stats](../core/include/liltview/core.h#L466) 是全局内存／任务快照。

后果：无法区分排队、实际 I/O、解码、解包、去马赛克、转换和背压；失败／取消的阶段数据也缺失，更不能用 core elapsed 推断 GPU 或实际可见耗时。建议可选、有界、关联 request／frame／processing 的诊断摘要，成功／失败／取消都覆盖，renderer／显示观测另行关联。关联方案 §12。

### R11 · P1（仅原生高精度／GPU 转换路径）· Renderer 也需要协同扩展

证据：[render::Pixels／ImageDesc／Capabilities](../app/renderer/renderer.hpp#L66) 没有像素格式／颜色／多平面描述；[validate_upload／image_storage_bytes](../app/renderer/validation.cpp#L7) 固定按 RGBA8 字节计算，readback 为 RGB8。

后果：core 即使交付 P010 或浮点像素，当前 renderer 也不能正确上传／转换／保留精度；只改 core 会形成接口断层。建议第一阶段在 CPU 转为已声明的 RGBA8 预览；后续 core 与 renderer 联合协商精度、平面、颜色／alpha 和处理职责，不能给 core 增加 GL／D3D 对象。关联方案 §4、§8。

## 可直接沿用的契约

- 单控制线程、submit 不读文件、接受后异步处理、同一请求不可变；参数调整用新请求与新 user_tag。
- INFO／像素／终结的排序、同请求只提交一个终结；旧请求不会因新参数被“就地修改”。
- 像素引用计数、事件借用、取消竞争、三阶段关闭；新增元数据和所有 plane 必须与拥有者一同存活。
- FIT／REGION 的未旋转坐标、方向由 UI 统一应用；格式适配器不重复旋转。
- 动画一轮内顺序 NEXT、RESET 新 epoch、core 合成完整显示帧、UI 控制时钟；不因新增格式引入超大动图目标。

## 契约漂移与待验证边界

审阅时，工作区的 query／codecs 已存在 REGION 路径，而部分 API／格式文档仍描述未实现；Runtime §7.2 的“动画 WebP／APNG 不提供播放 API”也与现有通用动画 API 的存在容易混淆。本次仅澄清该动画表述为“API 可复用，但具体后端未实现”；REGION 能力与实现状态仍应在当前超大图开发验收时统一。本次不将工作区代码状态改写为已验收，也不以陈旧描述否定当前新增路径。

本 review 没有选择／锁定新依赖，没有确认每个后端的安全工作集、打断粒度、ICC／HDR 准确度或 GPU 支持。逐格式落地路径、字段要求、分期和验收用例记录于 [CORE_FORMAT_EXTENSION_PLAN.md](CORE_FORMAT_EXTENSION_PLAN.md)，开发状态继续是 [待开发](FORMAT_ROADMAP.md)。
