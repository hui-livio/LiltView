# 8K 随机彩色棋盘格：首次加载上传策略对照

日期：2026-11-13。这是新实现两种上传策略的对照，不是与重构前版本的 A/B；不能据此声称相对旧版本整体加载加速。

## 环境与输入

- Windows 10 19045；CPU：13th Gen Intel(R) Core(TM) i5-13400；MSVC Release（/O2）；SDL_Render 自动选择 direct3d11。未直接调用 OpenGL 或 Direct3D。
- 隐藏 SDL 窗口 1600×1000；图片 GPU 预算 128 MiB。未采集显卡型号 / GPU 执行时间，不将 CPU 提交或 present 返回冒充实际扫描呈现。
- PNG：8192×8192 RGB，RGBA 工作量 256 MiB；16px 棋盘格按明暗交替，每格独立随机 RGB，512px tile 不命中整块纯色捷径。seed=20261113。
- SHA-256：`abbc629bcd2ea056858ce54e30f34c09e4a2e68888efd2f25f8b783cf52f1fa8`；文件 1517605 字节；Pillow 13.0.0。
- 每样本新进程；OS 输入页缓存预热、场景匿名缓存重建；两种模式交替顺序。不是冷磁盘或前台实屏测试。

## 主组：请求 VSync=1，目标帧周期 16.667 ms，每模式 30 次

| 指标 | pipeline | batch |
| --- | ---: | ---: |
| 首屏就绪中位数（ms） | 2319.91 | 2437.98 |
| 首屏就绪 P95（ms） | 2357.35 | 2455.15 |
| 首屏 present 返回中位数（ms） | 2321.06 | 2438.32 |
| 首次上传中位数（ms） | 54.00 | 2336.97 |
| 加载提示 present 返回中位数（ms） | 3.77 | 3.56 |
| 每轮最慢上传片段的中位数（ms） | 15.76 | 101.70 |
| 每轮最慢主线程 work 帧的中位数（ms） | 16.34 | 102.20 |
| 所有轮次最慢 work 帧（ms） | 28.69 | 117.72 |
| 每轮最长探针事件延迟的中位数（ms） | 18.64 | 102.21 |
| 完成队列峰值中位数（MiB，含元数据） | 0.19 | 16.10 |
| 进程峰值 RSS 中位数（MiB） | 90.18 | 116.72 |

该调度条件下流水首屏中位数比集中上传少 118.07 ms（约 4.84%）。更明显的差别是避免约 100ms 的集中上传长帧；但流水最坏仍有 28.69ms，不能声称无卡顿或硬性 2ms 上限。

batch 每次只有少数长帧，帧 P95 甚至可能更低；因此必须同时看最大值和事件延迟，不能只看 P95。work 包含 pump、布局、绘制提交和 present，不含为目标帧周期主动 sleep。

## 吞吐诊断组：VSync=0、无目标帧周期，每模式 10 次

| 指标 | pipeline | batch |
| --- | ---: | ---: |
| 首屏就绪中位数（ms） | 1721.40 | 1679.11 |
| 每轮最慢 work 帧中位数（ms） | 17.10 | 101.71 |

取消帧周期限制后，batch 首屏中位数略快（约 2.46%），仍保留约 100ms 的长帧。说明这是吞吐与交互响应的权衡，不存在“流水在所有条件下一定最快”的结论。日常默认 pipeline，batch 保留为配置对照。

## 正确性与资源口径

- 80 次全部通过：256 个 tile、两种模式基础 / 概览上传均为 16 MiB、Core 保留输入峰值不超过 2，半成品可见状态计数为 0。
- pipeline 首次上传早于观测到 Core COMPLETE；batch 首次上传在 COMPLETE 与 CPU 准备成功后。首屏前所有必要缓存已完成，后续相机操作不重解码。
- 80 次最终 SDL 画面 RGB 的 FNV-1a 都为 `211789602916202268`。这验证两模式输出一致，不等于独立色彩科学基准。
- RSS 是进程高水位，含初始化和最终验证读回；进程 CPU 是测量点累计 CPU，不是单阶段纯解码 CPU。Core elapsed 包含排队 / 背压，阶段墙钟可能重叠，不能相加各项 P95。
- 输出队列上限：pipeline 8 MiB，batch 64 MiB；原始 Core backing 独立受 Core 预算约束。所有活动 / 退休场景共享 16 GiB 磁盘总准入。

## 复现与原始数据

```powershell
cmake --preset msvc-release
cmake --build build/msvc-release --target liltview liltview_loading_benchmark --parallel 6
python benchmarks/loading_compare.py --exe build/msvc-release/app/liltview_loading_benchmark.exe --runs 30 --output build/loading-comparison-8k
python benchmarks/loading_compare.py --exe build/msvc-release/app/liltview_loading_benchmark.exe --runs 10 --vsync 0 --cadence-ms 0 --output build/loading-comparison-8k-unpaced
```

- 持久化样本：[samples.json](samples.json)；汇总：[主组](summary-paced.json)、[不限帧周期组](summary-unpaced.json)。
- 本次本地完整阶段日志、逐帧 CSV、生成 PNG 与 YAML 位于 `build/loading-comparison-8k/` 和 `build/loading-comparison-8k-unpaced/`。
- 完整应用另以深色 pipeline / 浅色 batch 加载该图，并验证 GIF：均成功退出。日志和截图位于 `build/loading-smoke/`；不混入上述统计。
- Linux/macOS、真实高熵照片、冷 OS 页缓存、真实前台点击与合成器呈现仍需另验。
