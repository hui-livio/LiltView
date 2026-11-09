# 图像渲染优化开发计划

更新：2026-11-09。**自定义图像 shader、RGB/YUV 直接渲染仅列入计划，本轮不实现。** 当前 OpenGL 后端仍通过 ImGui OpenGL3 backend 绘制 RGBA8 纹理，不新增 GLSL、GPU plane 资源、原始格式入口或公共像素结构字段。

相关：[当前 review](RENDERING_REVIEW-2026-11-09.md)、[性能待办](PERFORMANCE_TODO.md)、[renderer 边界](RENDERER-v0.1.md)、[core 扩展契约](CORE_FORMAT_EXTENSION_PLAN.md)、[原始像素输入](RAW_PIXEL_INPUT.md)。

## 1. 不变的显示契约

1. 静态图以一次原始分辨率解码形成不可变场景，允许最终原始像素行带先完成、先显示；不以放大 FIT 缩略图冒充高清。
2. 加载完成后，缩放 / 平移 / EXIF 变换只更新坐标、矩阵、UV 和采样选择，不提交新的图片解码任务，不重新生成像素或 LOD。
3. 已驻留 GPU 的纹理不因普通相机操作重新上传。显存不足时只恢复本地已生成的分块 / LOD 缓存，不重新解码源图；缓存恢复不宣传为严格零开销。
4. core 不持有 GL context、shader、纹理或 GPU fence；GPU 操作限定渲染线程，跨线程传递有界、不可变、版本明确的资源描述及引用。
5. 不以无界 RGBA 常驻、跳过颜色解释或丢弃 alpha 换取速度；预算 / I/O 错误必须可诊断，不静默降成低清内容。

## 2. P0：先建立可复现的分段基线（待开发）

- [ ] Release 构建记录文件版本、尺寸 / 格式 / progressive / interlace、窗口 / DPI、WSLg / X11 / Wayland、实际 GL_VENDOR / GL_RENDERER / GL_VERSION、硬件和驱动。区分真实 GPU 与软件渲染。
- [ ] 测量探测 / 文件读取、解码、行带等待、CPU LOD、压缩 / 磁盘缓存、纹理创建 / 上传、GPU 绘制 / present。
- [ ] 记录首个原始行带、整幅原始场景完成、100% 就绪、GPU 缓存恢复的端到端延迟；交互帧耗时 P50 / P95 / P99、draw calls、上传 bytes 和缓存命中率。
- [ ] 冷 / 热文件缓存分别测量，包含真实文字 / 图表 / 照片、透明图、16K / 32K 棋盘。不得把功能测试时长当成与 VS Code 的性能对比。
- [ ] 将测试中的“删除原图后继续缩放 / 拖动”保留为无重解码回归；对常驻纹理还应断言无额外上传。

**准入**：确认耗时来自 CPU 解码 / 颜色转换 / 缓存，还是 GL 上传 / draw submission，再决定后续阶段顺序。换 shader 不等于加速 PNG inflate 或 JPEG 熵解码。

## 3. P1：当前 RGBA8 链路先完成的优化（待开发）

- [x] 交互缓存读取 / 解压使用后台 reader：32 MiB RAM LRU、64 项待处理请求，替换过期排队任务、完成唤醒；按视口索引和限量上传预热邻居，见 [应用说明](../app/README.md)。
- [ ] **首次加载生成**的 CPU LOD、压缩和缓存写入仍需迁出 UI；缓存恢复后台化不代表首次生成后台化。单次 GL 调用仍没有硬实时上限。
- [ ] 统一 app 场景索引、LOD 描述符、压缩暂存、旧 / 新场景和本地缓存的内存 / 磁盘账本；core CPU 账本不等于整个进程预算。
- [ ] 解决强缩小时 atlas 只有线性 minification 的混叠风险：使用首次解码时已生成的较低 LOD，或批量完成后建立 atlas mip 链。不能在每次缩放时重新计算像素，也不能在每块更新后重建整张 atlas mip 链。
- [x] 完整 tile 用带像素创建接口，避免透明初始化后立刻覆盖和两次 mipmap 生成；局部更新的透明初始化保持不变，stride / 状态恢复有回归。
- [ ] 继续评估相邻更新合并、跨 tile 批处理和驱动上传时延；不因一次初始化优化就宣称所有上传瓶颈已解决。
- [ ] 持续验证 FIT atlas 首次写入、不同源替换时显存准入、纹理 / fd 释放和非均匀内容，不能只用大块纯色掩盖错误。

## 4. P2：独立图像 pass / 自定义 shader（明确延期）

- [ ] 保留 ImGui 自身 UI shader；单独设计 image pass，不把原始 plane 句柄强塞进现有单纹理 `ImTextureID`。
- [ ] 先用现有 RGBA8 实现视觉等价的图像 pass，验证相机矩阵、EXIF、物理像素对齐、剪裁和 straight-alpha；不能先同时更换 shader、像素格式和颜色策略。
- [ ] 比较直接 canvas pass 与 viewport-size FBO + ImGui 合成的成本；FBO 必须计预算，不能创建整幅超大源尺寸 render target。
- [ ] 第一版以现有 OpenGL 3.3 能力为边界，采用 vertex / fragment shader；compute shader、持久映射 PBO 等仅作为可选增强，不直接提高最低版本。
- [ ] 明确 GL program / VAO / buffer / texture unit / sampler / blend / scissor / framebuffer / pixel-store 状态恢复、context 生命周期和失败回退。
- [ ] 准入：旧 RGBA8 路径与新 pass 相同输入逐像素一致或满足声明的颜色容差，性能数据证明收益后再切换默认后端。

## 5. P3：RGB / 灰度直接上传（待开发）

- [ ] renderer 增加明确的 RGB8 / RGBA8 / R8 输入协商，RGB 的 alpha 为 1，灰度的通道扩展规则显式定义；没有透明度的源不必先生成整张 RGBA 暂存。
- [ ] RGB8 本身可由纹理采样器读取，不把“RGB8 支持”误称为必须采用复杂 shader 的功能。RGB/BGR 的重排与存储格式分别定义。
- [ ] 任意合法行跨度、奇数宽度、`GL_UNPACK_ALIGNMENT` / `ROW_LENGTH` 逐格式处理，不能继续硬编码 width×4、stride/4；必要逐行上传而非整图打包。
- [ ] GPU 预算按保守格式占用估计；实际显存和驱动行为用测量验证，不承诺 RGB8 一定比 RGBA8 少占 25% 实体显存。
- [ ] core 仅在 codec 能高效提供时输出原生格式；保留 RGBA8 fallback。当前 ABI 2 的 `lv_pixel_view` / `Pixels` 结构不偷加字段，按扩展方案协商新接口 / ABI。

## 6. P4：YUV 多平面采样与 GPU 颜色转换（待开发）

- [ ] 第一批明确支持子集：8-bit I420 / NV12；后续独立验收 YV12 / NV21、4:2:2 / 4:4:4、packed 布局、高位深 P010 / planar 10-bit。不以“支持 YUV”笼统表示全部布局。
- [ ] 候选纹理表示：I420 为 Y / U / V 三个单通道平面；NV12 为 Y 单通道 + UV 双通道。plane 的尺寸、offset、有效行字节、stride、UV 顺序、位深 / 容器对齐和边界分别定义并受检。
- [ ] shader 通过显式颜色描述解释 matrix、full / limited range、chroma siting、primaries / transfer；分别覆盖 BT.601 / 709 / 2020 适用子集，不能统一硬编码一个 YUV→RGB 矩阵。未指定必需元数据时明确默认或拒绝，不能默猜。
- [ ] JPEG 如需直接 YCbCr 输出，先评估现有 libjpeg API 的 raw component 输出、MCU / 下采样 / progressive 行为与 SIMD 成本。当前构建 `WITH_TURBOJPEG=OFF`，不能直接假设 TurboJPEG API 已可链接。
- [ ] PNG 的解压工作与 YUV shader 无关；不要把已有 RGB / RGBA 输入先转成 YUV。也不把 shader 颜色转换称为 GPU JPEG / PNG 解码。
- [ ] 颜色 / 多平面输出是与 raw 输入布局相关但不同的接口；raw 参数仍须满足 [输入契约](RAW_PIXEL_INPUT.md)，有界复制跨线程描述，所有权 / 版本 / 取消与现有不可变事件一致。
- [ ] GPU 采样时完成色度重建和颜色转换，不在每次视口操作前生成新的 CPU RGBA 图片；已生成的纹理 / plane / LOD 继续缓存。

## 7. P5：条件性的异步上传（仅在测量后实施）

- [ ] 对比直接 `glTexSubImage2D`、多缓冲 PBO / fence；不假定 PBO 必然更快。
- [ ] 如果 GPU 在 `update_image` 返回后仍使用 CPU / staging 数据，必须新增明确的引用保留、完成通知和失败释放契约，不能沿用“返回即可 release pixels”而实际延后访问。
- [ ] PBO / GPU fence / 复用池全部计入预算，保留无扩展 / 软件 OpenGL 的同步回退。

## 8. 验收矩阵

- [ ] 功能：FIT、100%、非整数缩放 / 拖动、HiDPI 1 / 1.25 / 1.5 / 2、EXIF 1..8、超出纹理边长、缓存淘汰 / 恢复。
- [ ] 像素：1-pixel 线条 / 棋盘、透明镂空 / 半透明边缘、RGB/BGR 色条、YUV 中性灰 / 黑白电平 / 彩条；不同 matrix / range 的 golden reference。
- [ ] 存储：不规则 stride、odd-size 的明确支持或拒绝规则、plane 尾部 padding、截断、溢出、错误 UV 顺序、P010 与 planar 10-bit 不混同。
- [ ] 生命周期：快速换图、上传失败、缓存 I/O / 显存不足、取消、shutdown、GL 状态 / fd / 纹理无泄漏；旧图和候选图不混版本。
- [ ] 性能：Release 分段前后对比和资源占用；常驻图片相机操作无源解码、无 CPU 重采样、无重复上传；缓存未命中只恢复已有内容。

所有项目在合入并通过相应测试前均保持“待开发”，不更新格式兼容表或宣称 RGB / YUV shader 已可用。
