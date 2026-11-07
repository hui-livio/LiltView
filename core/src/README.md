# 核心实现分区

已实现 Linux 常规 JPEG DISPLAY / FIT 链路和所有公共函数入口。未接入的 PREFETCH、REGION、缓存、多 worker 与其他解码器返回明确不支持，不提供伪成功实现。

现有分工与后续扩展：

- `api/config.c`：配置 / 请求初始化和 ABI 查询。
- `api/engine.c`：创建、参数校验、单 worker 调度、取消与三阶段关闭。
- `engine/objects.c`：预算预约、事件池、像素引用计数、源身份与队列背压。
- `io/`：本地数据源、文件身份和分批读取。
- `codecs/jpeg.c`：有界输入、8 位 RGB / 灰度 JPEG、EXIF、缩放解码、双线性 FIT 和最终 tile 发布。
- `cache/`：CPU 缓存与可选磁盘缓存。
- `platform/file.c`：Linux 常规文件、版本检查和单调时钟；Windows 文件适配待实施。线程使用 C 线程 API。

核心不链接 ImGui、spdlog、Catch2 或网络库。新增 `.c` 文件由 C23 编译器编译。

libjpeg-turbo 3.1.2 由固定 submodule 独立构建为静态库。当前解码器内部内存使用保守预约，未拦截其分配，不把账本当进程 RSS 硬上限。运行约束详见 [Core API 当前实现范围](../../docs/CORE_API-v0.1.md#12-当前-jpeg-实现范围)。
