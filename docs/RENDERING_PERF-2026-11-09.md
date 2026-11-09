# 真实大图缩放 / 平移卡顿复现（2026-11-09）

## 输入与边界

用户反馈“正在恢复缓存”时卡顿，真实输入来自 `/mnt/d/share/images`。本次没有修改、重编码或把这些文件加入仓库。

- `moban.png`：12144×10384，RGB8，非交错。
- `1moban.png`：17952×13200，RGB8，非交错。
- `1_1_01.jpg`：12122×10866。
- 环境：Arch / WSLg，SDL **x11**，`GL_VENDOR=Microsoft Corporation`，`GL_RENDERER=D3D12 (NVIDIA GeForce RTX 3060)`，`GL_VERSION=4.6 (Core Profile) Mesa 26.2.6-arch1.1`。不是根据 offscreen DRI warning 推测的软件渲染。
- Release、1600×1000 隐藏窗口、实际 swap interval=0、128 MiB images 预算。无截图读回，不含应用菜单；不是完整应用窗口的 FPS。首轮实际驱动探测 / Debug 与 Release 构建对照之后，最终三文件和 direct 对照顺序运行，没有并发 benchmark / CTest。
- 操作：初始 FIT 全图完成→100%→120 帧水平往返平移（每帧16px）→重复平移→逐次 12 步缩小、12 步放大；每次离散缩放等待目标完成。平移帧间 8ms / 恢复帧间 1ms 节流计入操作总延迟，不计入单帧耗时。
- 单次运行的诊断数据，不是重复实验的置信区间。没有控制 Windows / Linux OS 页缓存，不把初始加载差异都归因于此补丁。

## 定位：UI 上传预算挡不住一次驱动调用

原先只在 GL 调用之间检查 2ms 预算；真实驱动的一次 client-pointer 256×256 RGBA 上传就能耗时约 12.6ms。

独立调用微基准（同 GL context，30 次均值）进一步分开分配、像素传输、mipmap 和 delete：

| 路径 | 分配 | 像素传输 | mipmap |
|---|---:|---:|---:|
| `glTexImage2D(nullptr)` + client-pointer `glTexSubImage2D` | 0.386ms | 12.589ms | — |
| `glTexStorage2D` + client-pointer `glTexSubImage2D` | 0.400ms | 12.689ms | — |
| 重复写同一纹理的 client-pointer 路径 | — | 12.460ms | — |
| `glTexStorage2D` + PBO `glBufferData` / `glTexSubImage2D` | 0.313ms | 0.478ms | — |
| client-pointer 上传后 `glGenerateMipmap` | 0.456ms | 12.556ms | 0.006ms |

这里测量的是 CPU API 调用时间，GPU 工作可能延后，不是完整 GPU 执行耗时。全图基准的 present 也单独记录，防止把上传代价转移到 present 就误判为加速。

曾尝试直接上传首次生成的完整 mip 链，真实图反而更慢；已撤回，最终补丁不包含新 mip-chain API / IO 或 shader。根因证据指向 client-pointer 像素传输路径，而不是“每次 mipmap 必然很慢”。

## 保留的改动

1. OpenGL3 默认通过有界 PBO 暂存像素。8 个槽、每槽最多 4 MiB，独立于 images 预算报告；只有 fence 完成才复用槽，不主动等待、不对 in-flight 槽无限 orphan。缓存恢复槽满就延后。
2. 原同步借用接口不变：返回前 `glBufferData` 已复制调用方字节，GPU 后续只使用 renderer 自有缓冲。非恢复调用 / 超大跨度保留 direct fallback，stride / 状态恢复 / 透明初始化 / 事务失败清理不变。
3. 应用 native tile 256→512px，降低中间比例下独立 texture / draw 数。core 公共默认仍是256，设备上限小于512时继续减小。
4. 增加手动 `liltview_viewer_benchmark`，不将用户文件绑定到 CTest。输出驱动、分段耗时、恢复 / 读取 / 停住帧计数、images 和 staging 计数。

## 实测结果

原提交（256px、直接上传）Release 的 `moban.png`：100% 冷恢复 **642.3ms**，首次平移帧 P95 **17.71ms**；第8步缩小等待 **1857.0ms**。Debug 同图100%为857.2ms，不能混用 Debug / Release 作优化倍数。

最终 PBO + 512px，在上述相同 viewer 工作负载下：

| 输入 | 100%冷恢复 | 首次平移帧 P95 | 首次平移等待 / 完全停住帧 | 重复平移额外读取 / GPU恢复 |
|---|---:|---:|---:|---:|
| moban.png | 89.3ms | 6.26ms | 2 / 0 | 0 / 0 |
| 1moban.png | 87.1ms | 5.64ms | 1 / 0 | 0 / 0 |
| 1_1_01.jpg | 69.1ms | 2.94ms | 2 / 0 | 0 / 0 |

`moban.png` 第8步缩小等待由1857.0ms降为280.3ms；下一步的冷层级仍需696.5ms。中间比例热绘制的 draw commands 从千级降低到几百级，例如一轮反向缩放首个细节视图从1162降为309。

上述最终三图暂存上传计数非零，没有走 direct fallback；暂存逻辑上限32MiB，images 依旧不超过128MiB。

### 同一 512px 构建的 direct 对照

通过 `LILTVIEW_BENCH_DIRECT=1` 禁用 PBO，保持其他参数不变。这个对照用来区分 PBO 收益和 tile 尺寸收益；不将256→512的收益全部归给 PBO。

| 同一512px构建，moban.png | direct | PBO |
|---|---:|---:|
| 100%冷恢复 | 706.4ms | 89.3ms |
| 首次平移帧 P95 | 10.00ms | 6.26ms |
| 首次平移最大帧 | 60.54ms | 9.36ms |
| 第1步缩小完成 | 346.6ms | 27.4ms |
| 第8步缩小完成 | 808.8ms | 280.3ms |
| 下一步冷层级完成 | 1485.9ms | 696.5ms |
| staged / direct 调用计数 | 0 / 892 | 892 / 0 |

注意：P95并不能揭示少数昂贵恢复帧，必须一起看最大帧和整次目标完成延迟。当前512px直接上传的单个原始tile耗时约48–52ms，比256px更慢；因此不能只改大tile、不修改上传路径。

## 正确性与仍未解决的范围

用户复测反馈：**卡顿仍未解决，但已有优化**。本次提交是上传阻塞与绘制数量的阶段性优化，不将该问题标记为已解决。

最终 Debug 全套 `ctest --test-dir build/dev --output-on-failure -j 6`：**128/128 通过，87.08s**。Release 应用与手动基准构建成功。测试通过说明正确性 / 生命周期回归，并不替代上述真实驱动交互测量。

- PBO / direct 两条路径分别覆盖 stride16/17/20、源字节立即清空、调用方 unpack PBO / pixel-store / texture binding 恢复，以及64次混合上传的像素 / 预算检查。
- 人工阻塞上传准入时，目标恢复被延后，旧完整视图不被部分替换；默认 covered-motion 测试跨过512px halo，继续验证不透明 / 半透明的相机推进、完整覆盖和像素对应。
- 原图删除后仍能恢复、资源释放、HiDPI、EXIF、16K / 32K 等原有回归继续保留。
- **不宣称彻底消除卡顿**：冷区域 / 冷 LOD 仍可能需要几十至几百毫秒，连续大跨度输入仍可能耗尽完整高清覆盖。大图初次 LOD / 压缩仍在 UI 线程；本轮源文件初始全图完成分别约17.9s、155.8s、13.7s，未完成源 IO / 探测 / 解码的全面拆分，也不能把 `/mnt/d` 的文件访问代价说成已解决。初始加载与缓存恢复要分开优化。
- 暂不实施自定义 shader、RGB/YUV plane、源图重新解码、低清放大替代、无界 RGBA 常驻。下一步是首次缓存生成后台化、冷层级批量 / atlas 驻留及预测预热，而非增加硬等待或隐藏状态提示。
