# 核心源码入口

当前支持 Linux 与 Windows 文件适配，使用 C23（MSVC 为 C17 与私有兼容层）、ABI 2、单共享 worker；七类格式的实际范围见[兼容表](../../docs/IMAGE_FORMATS.md)。调用、所有权、预算、取消和关闭统一维护在 [Core 契约](../../docs/CORE_CONTRACTS.md)，完整声明见 [core.h](../include/liltview/core.h)。

| 路径 | 职责 |
| --- | --- |
| `api/config.c` | 配置 / 请求初始化、ABI 查询 |
| `api/engine.c`、`api/animation.c` | 参数校验、请求 / 会话入口、取消、三阶段关闭 |
| `engine/worker.c`、`engine/animation.c` | 静态与动画任务调度、会话状态和清理 |
| `engine/objects.c` | 资源账本、事件池、像素引用、源版本及队列背压 |
| `engine/cache.c` | 完整静态结果内存 LRU、版本核验重放、真实 PREFETCH 与资源驱逐 |
| `codecs/image.c`、`codecs/raster.c` | 内容探测、输出准入、行带 / tile 发布、受检采样 |
| `codecs/` 其他文件 | 各格式解码、GIF 持久合成、格式限制与清理 |
| `platform/file.c` / `platform/file_win32.c` | Linux / Windows 常规文件、版本核验和单调时钟 |

静态支持 FIT / NATIVE，JPEG / PNG / 未压缩 BMP / TIFF 另有 REGION；NATIVE 的区域 codec 可提前发布完整原始行带。最终像素交付移交引用或共享 backing，不为事件拆分深拷贝。

Core 不依赖 UI、renderer、翻译、spdlog 或网络。codec 使用固定源码静态构建；工作集按保守 allowance 准入，不承诺进程 RSS 硬上限。已实现可选静态内容缓存和 PREFETCH；磁盘内容缓存、多 worker 与通用完整动画会话缓存仍待实现。SVG 仅支持[基础静态 profile](../../docs/IMAGE_FORMATS.md#4-svg-静态-profile)，复杂 SVG 不支持。应用场景缓存见[渲染说明](../../docs/RENDERING.md)。
