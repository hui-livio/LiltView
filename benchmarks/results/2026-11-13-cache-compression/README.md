# 内存缓存压缩开关：4K / 8K / 16K 实测

日期：2026-11-13。基线为同一工作区 Release 实现，仅切换 `static_cache_compression`，不是旧版 zlib / 磁盘实现的 A/B。未提交源码状态与测试二进制 SHA-256 见 `summary.json`。

## 方法与环境

- Windows 10 19045，Intel Core i5-13400（10C / 16T），物理内存约 28.8 GiB；系统有 RTX 3060 和远程虚拟显示适配器。实际 SDL_Render driver 为 `direct3d11`，不凭设备列表推定活跃 GPU。
- MSVC Release，隐藏 1600×1000 SDL 窗口，实际 VSync=1，帧周期 16.667 ms，GPU 图片预算 128 MiB。无其他同时运行的构建 / 基准。
- 用户本地可信原图的居中裁图，不缩放、RGB PNG、compress_level=1。分辨率分别为 3840×2160 / 7680×4320 / 15360×8640；宽高、文件长度与 SHA-256 见 inputs.json，个人图片不入库。
- 每个尺寸 / 模式各 5 个新进程，共 30 次，逐轮交替 on/off 顺序。文件缓存未清空，输入刚生成 / 已读取，不能作为冷磁盘结果。每次场景缓存是新建的纯内存缓存。
- 主指标为打开请求至完整首屏 ready，另列 present 返回；不含进程启动、窗口创建、图像 readback 或后续渲染探针，不等于显示器扫描完成。所有表格使用各指标独立中位数。

## 完整首屏

| 图像 | 压缩开启 ready (ms) | 压缩关闭 ready (ms) | 关闭相对变化 | 开启 / 关闭 present 返回 (ms) |
|---|---:|---:|---:|---:|
| 3840×2160 | 374.97 | 408.24 | +8.87% | 376.07 / 409.34 |
| 7680×4320 | 878.39 | 876.95 | -0.16% | 879.06 / 877.40 |
| 15360×8640 | 3379.05 | 3366.13 | -0.38% | 3379.97 / 3367.31 |

4K 两种模式均走直接 Core → UI → GPU 上传，worker=0、压缩=0、CPU 场景缓存=0；开关本来就不改变这条路径，差异是重复测量与调度波动。8K / 16K 各使用最多 2 个有界 CPU worker，input_peak≤8。

## CPU 阶段与加载内存

| 图像 | 开启压缩累计 (ms) | 关闭压缩 / 解压 (ms) | 开启 / 关闭逻辑缓存 (MiB) | 开启 / 关闭加载峰值 RSS (MiB) |
|---|---:|---:|---:|---:|
| 3840×2160 | 0.00 | 0.00 / 0.00 | 0.0 / 0.0 | 105.0 / 104.9 |
| 7680×4320 | 404.27 | 0.00 / 0.00 | 118.2 / 168.8 | 259.9 / 246.6 |
| 15360×8640 | 1575.38 | 0.00 / 0.00 | 477.9 / 675.0 | 781.2 / 756.4 |

- worker 时间按各线程累计并与 Core / UI 流水重叠，不能把压缩累计量直接从完整加载墙钟时间减去。Core 时间包含读取 / 解码 / 背压，当前数据不能单独断言纯解码 CPU 占比。
- LZ4 逻辑缓存更小，但当前编码输出保留 compressBound 容量；缓存总账计 capacity + 保守元数据，cache_bytes 仅是有效长度。因此本样本 raw RSS 反而略低，不应宣称开启压缩必然降低实际内存。后续收紧压缩输出容量会在分配 / 复制和内存之间引入新权衡。
- `load_peak_rss_bytes` 为首屏后、渲染探针前的进程峰值；`peak_rss_bytes` 包含后续 100% 恢复。它们不是单一缓存分配量，也不是统一应用预算。

## 渲染与 100% 恢复

| 图像 | 开启 / 关闭 FIT 帧 CPU work p95 (ms) | 开启 / 关闭 100% 恢复 (ms) | 开启 / 关闭加载期单轮最慢 work 帧中位数 (ms) |
|---|---:|---:|---:|
| 3840×2160 | 1.03 / 0.94 | 17.90 / 17.79 | 7.12 / 6.89 |
| 7680×4320 | 0.49 / 0.71 | 140.88 / 141.23 | 14.94 / 19.83 |
| 15360×8640 | 1.20 / 1.42 | 106.37 / 122.45 | 15.11 / 16.69 |

FIT 采样 60 帧，随后切换 100% 并等待当前中心视口细节完整，再采样 30 帧；不是将整幅 16K 原图所有细节上传 GPU。100% 时间包含帧周期、后台准备和上传；关闭压缩并不保证这个 UI 门槛变快。原始缓存读取保留同一 C 内存块，codec 时间为零，但驱动与调度成本依旧存在。

`cache_decode_ms` 包含加载、可选预读和 100% 探针期间的 reader 累计量，不是单纯首屏解压或 GPU 时间。完整 per-run CPU work / submit / present 分位数、RSS、source_bytes 和阶段量保存在 runs/*.json；frame CSV 和日志仅保留 build。

## 校验与结论

- 30 次均没有可见半成品帧；各分辨率所有开关轮次 FIT framebuffer hash 相同。自动断言配置生效、关闭时压缩 / 解压为零、有界 worker / 输入 / 输出，不把 hash 一致冒充独立色准 oracle。
- 两条场景缓存路径均只用内存，cache_disk_bytes=0 是实现路径声明 / 计数，不是 ETW 磁盘采样；仍会读取源文件、保存配置，OS 也可能分页。
- 本机这批 4K PNG 已在 1 秒内完整首屏；8K / 16K 关闭压缩的端到端收益很小，不能把取消压缩视为数量级加速。16K 仍非秒开；后续需继续分解 Core 读取 / 解码 / 背压和上传调度。
- 按需求默认仍为 true，隐藏配置供不同图片、机器与内存条件实测选择。不外推到 JPEG / 高熵照片、4096²、其他显卡、前台实屏或跨平台。

## 复现

```powershell
# 在 MSVC x64 开发环境，仓库根目录
cmake --preset msvc-release
cmake --build build/msvc-release --parallel 6 --target liltview liltview_loading_benchmark
python benchmarks/cache_compression_compare.py --exe build/msvc-release/app/liltview_loading_benchmark.exe --output build/cache-compression-4k-8k-16k --source D:/share/images/1moban.png --runs 5
```

配置分别是 `static_upload_mode: pipeline` + `static_cache_compression: true/false` + `log_level: info`，不修改用户的实际 settings.yaml。`--source` 需要 Pillow；现有输入可改为 `--inputs <目录>`。源 / 裁图不分发，可用 inputs.json 对照本地输入。

## 构建与应用回归

- MSVC 开发构建 / Release 构建成功。最终完整 CTest 361 项：359 通过、2 项既有 POSIX symlink 场景跳过、0 失败。中途一次既有 Windows 最大化自定义区域测试瞬时失败；单独连续 3 次与全量复跑均通过，未改动窗口区域实现。
- 实际 `liltview.exe --smoke-test` 使用隔离配置目录，在 4K / 8K / 16K 分别开启 / 关闭，6 次均完整显示、正常退出；日志确认 false 使用 raw-rgba、缓存压缩 / 解压为零。单轮数据见 `app-smoke.json`，真实应用窗口 / UI 不同于 1600×1000 基准，未混入上表统计。
- LZ4 子模块固定版本校验、dav1d-ref 受控子集 hash、`git diff --check` 通过。未执行提交。
