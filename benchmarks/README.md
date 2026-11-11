# 性能测量

本目录存放性能测试方法和样本清单；手动 viewer 基准位于 `tests/integration/viewer_benchmark.cpp`，复现入口与历史测量见[渲染说明](../docs/RENDERING.md)。大图片与机器生成的输出保留在本机，不直接提交到仓库。

完整方法见 [TEST_PLAN.md](TEST_PLAN.md)。AVIF、JPEG XL、HEIF / HEIC、APNG、动画 WebP、ICO / CUR、QOI、JPEG 2000、OpenEXR、相机 RAW / DNG，以及原始 RGB / 灰度、YUV、Bayer 均已纳入后续性能矩阵；对应实现 **待开发**，新输入 **待补充**，数据 **未测量**。YUV 包括 4:4:4 / 4:2:2 / 4:2:0、YUYV / UYVY 等具体排列；原始输入必须记录宽高、格式、stride / offset、位深、颜色和 CFA 等适用参数。

已生成的棋盘格随机色块素材见 [inputs/README.md](inputs/README.md)，清单共 62 个样本，覆盖普通静态图、超大静态图和普通 GIF 动图；SVG 只覆盖基础几何的元素数量分档，不代表复杂 SVG 支持。逐样本尺寸、编码参数、文件大小、SHA-256 见 [manifest.json](inputs/manifest.json)，输入文件核验记录见 [validation.json](inputs/validation.json)。生成时图片位于 `inputs/static/normal/`、`inputs/static/large/` 和 `inputs/animation/gif/`，按格式再分目录；这些本机素材不随 Git 克隆提供，先核对是否存在及是否匹配清单；`inputs/.gitignore` 将图片排除在 Git 提交之外。

测量首个有效画面、目标视图完成、解码耗时与吞吐量、像素处理、纹理上传、CPU / GPU 绘制耗时，以及 CPU / 内存 / GPU 占用。冷缓存与热缓存分开记录，并记录硬件、驱动和依赖版本。输入核验不代表应用兼容性或性能测试通过，基准不等同于 Catch2 正确性测试。

原始像素不把无压缩解码记为零成本：RGB 重排、YUV 转换、Bayer 解包 / 去马赛克 / 颜色处理独立计时，再与上传和绘制关联。阶段重叠时保留时间线，不相加各阶段 P95 冒充端到端耗时。

当前性能缺口与验收条件见[开发计划](../docs/DEVELOPMENT_PLAN.md)。后台缓存恢复与有界 PBO 已实现，首次缓存生成和冷 LOD 仍需优化。与 VS Code 的速度差距目前来自用户体感反馈，尚无同条件量化对比结论。
