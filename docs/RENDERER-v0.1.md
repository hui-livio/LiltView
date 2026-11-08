# LiltView 渲染模块设计 v0.1

- 日期：2026-11-07。
- 状态：统一 renderer 模块、OpenGL 3.3 后端和JPEG / PNG / SVG / WebP / BMP 上传已实现；DX11 尚未实现或在 Windows 验证。
- 目标：将图片纹理、GPU 能力、帧提交与 Dear ImGui 绘制后端从应用生命周期中分离。
- 相关契约：[首版设计](DESIGN-v0.1.md)、[Core API](CORE_API-v0.1.md)、[Core Runtime](CORE_RUNTIME-v0.1.md)。

## 1. 边界与模块位置

渲染器是可单独构建和测试的 C++ 模块 `LiltView::renderer`，首版实现目录 `app/renderer/`。模块有一个跨平台公共 C++ 接口，后端按构建配置选择。它不依赖窗口输入处理、文件浏览、图片解码、C core、应用设置、spdlog 或菜单模型。

```text
app/application.cpp        窗口、输入、ImGui 上下文、帧生命周期编排
app/platform/              SDL3 窗口 / 输入；后续可增加 Win32 窗口 / 输入
app/renderer/              统一 renderer API、工厂、纹理句柄与能力结构
  backend/opengl/           当前 OpenGL 3.3 + ImGui OpenGL 后端
  backend/d3d11/             后续 D3D11 + ImGui DX11 后端
app/viewer/                 图片请求、缩放平移、加载状态与 GPU image handle
core/                       C23：文件、图片解码 / SVG 栅格化、像素结果和任务生命周期
third_party/imgui/          ImGui 公共绘制命令及各自平台 / 渲染适配器
```

平台适配负责系统窗口和输入以及 ImGui 平台后端；渲染后端负责图形上下文 / 设备、交换链目标、ImGui renderer backend、图片纹理、上传与 present。Dear ImGui 官方将平台和渲染后端分别定义；Win32 平台后端可搭配 DX11 renderer backend。[1]

当前组合为 SDL3 + OpenGL 3.3；未来 Windows 组合为 Win32 + D3D11。UI、viewer、C core 与 renderer 公共接口不因图形 API 改变。SDL3 + D3D11 不是本项目 Windows 移植目标。

## 2. 后端选择

`LILTVIEW_RENDERER_BACKEND` 是 CMake 显式选择项；首版 Linux 默认 `OPENGL3`。Windows 阶段增加 `D3D11` 实现及目标链接。一个 build tree 选择一个后端，避免 renderer backend、平台窗口和图形库配置不一致；切换后端使用独立 CMake preset 或 build directory，不做运行时自动探测 / 静默降级。

configure 阶段验证系统与选项组合：Linux 首版接受 `OPENGL3`；Windows 在 D3D11 代码接入并验证前不宣称 `D3D11` 可用。请求未实现的后端时配置失败并说明原因。

| 能力 | OpenGL 3.3 | D3D11 |
| --- | --- | --- |
| ImGui renderer backend | OpenGL3 | DX11 |
| 设备与输出 | SDL GL context、默认 framebuffer | D3D11 device / immediate context、DXGI swap chain / render target |
| 图片资源 | GL texture object | D3D texture + shader resource view |
| 部分像素更新 | 纹理子区域上传 | texture subresource update |
| present | SDL GL swap | DXGI present |
| 尺寸限制 | 查询 `GL_MAX_TEXTURE_SIZE` | 查询实际 device feature level / format 能力 |
| UI 可见资源 | 不透明 `ImageHandle` | 同一接口，不传 SRV 指针 |

能力必须来自初始化后的实际设备查询，而非写死厂商规格。DXGI resize 时必须释放旧 back buffer 的引用后重建输出目标；D3D11 更新策略遵循其资源更新契约。[2][3]

## 3. 公共接口设计

接口位于 `app/renderer/renderer.hpp`，只暴露后端无关的标准 C++ 类型、ImGui 公共绘制类型和不透明句柄。公共头文件不包括 `SDL_opengl.h`、D3D11、DXGI、`HWND`，也不暴露 `GLuint`、`ID3D11Device*` 或 SRV。

```cpp
enum class Backend { opengl3, d3d11 };
enum class SurfaceKind { sdl_window, win32_window };
struct Image { uint64_t owner; uint32_t slot, generation; };
struct Pixels { const uint8_t *data; uint64_t byte_count, stride_bytes; Size size; };
struct Config { SurfaceKind surface_kind; void *surface; uint64_t image_budget_bytes; bool vsync; };

class Renderer {
public:
    virtual ~Renderer() = default;
    virtual Capabilities capabilities() const = 0;
    virtual Size framebuffer_size() const = 0;
    virtual Image create_image(ImageDesc) = 0;
    virtual void update_image(Image, Rect destination, Pixels source) = 0;
    virtual void destroy_image(Image) noexcept = 0;
    virtual void begin_frame() = 0;
    virtual void render_frame(ImDrawData *) = 0;
    virtual void present() = 0;
    // add_image / add_image_quad build draw-list commands; readback is for tools/tests.
};
```

该片段为职责示意；实际签名以 [`renderer.hpp`](../app/renderer/renderer.hpp) 为准。实现使用 Image（owner + slot + generation）、Pixels、ImageDesc 和 Config；create_renderer 返回 unique_ptr，失败抛带 ErrorCode 的 render::Error，底层分配失败可抛 bad_alloc。应用负责捕获，C core 不用异常。render_frame 和 present 分开，readback 在二者之间；add_image_quad 统一表达 EXIF UV 变换。公共接口兼容 C++20 / C++23。

公共 capabilities 报告 backend ID、最大纹理边长、GPU images 总预算与当前分配量；framebuffer_size 另行返回物理像素尺寸。OpenGL 每帧查询窗口大小，D3D11 后续在同一边界处理 resize。图形设备限制与应用分配的 GPU image bytes 分别管理；viewer 采用二者允许范围中的较小者。预算是 renderer 账本，不宣传为系统显存硬上限。

统一 image 接口只包含二维 RGBA8 纹理和矩形更新，不暴露通用 render pass、shader、设备枚举或底层图形句柄。未来大图 viewer 可持有多个 image handle 并复用此 API，无需首版实现虚拟纹理框架。

### 3.1 图像上传

- `create_image` 分配指定尺寸并初始化透明像素，未上传区域不暴露未定义显存；不放大或解码源图。
- `update_image` 检查源大小、行步长、`byte_count` 和目标矩形，所有地址计算都受检；目标矩形必须落在纹理边界内。
- 所有纹理创建 / 更新 / 销毁都发生在 render thread。`update_image` 返回后不再读取调用者的 data；后端若不能直接完成复制，应先复制到 renderer 持有的 staging 内存。调用方可以随后释放 core event / pixels。
- 一张纹理可以更新多个矩形；首版 JPG 将 core 最终 tiles 分帧更新到一张 FIT 纹理。默认线性过滤，边缘 clamp。UV、FIT、缩放和平移由 viewer 决定，core 像素保持未旋转的左上原点。
- alpha 遵循 core straight-alpha 约定。透明图像的棋盘背景由 viewer 绘制，renderer 不修改像素值。
- 句柄使用 owner + slot + generation；销毁后代数递增，耗尽不复用。访问失效 / 外来句柄抛 invalid_handle，destroy 对失效句柄幂等。当前帧已引用的 image 必须在 render_frame 完成后销毁，后端处理 GPU 指令中的资源引用。

core 像素同步上传后即可释放，不由 renderer 保留。后续如测量证明上传瓶颈，再添加 renderer 自己所有的 PBO / D3D staging，不改变调用方像素借用规则。

### 3.2 帧时序

```text
renderer.begin_frame()
ImGui platform backend NewFrame()
ImGui::NewFrame()
  UI 布局；renderer.add_image(...) 写入普通 ImGui draw list
ImGui::Render()
renderer.render_frame(ImGui::GetDrawData()) // clear、ImGui backend
renderer.readback()                        // 可选截图 / 测试
renderer.present()
```

context / immediate context 及 ImGui renderer backend 仅在 render thread 使用。`add_image` 只写 UI 绘制命令，不立即提交 GPU 操作。present 不等待解码或 core 任务；VSync 由后端配置控制。

ImGui platform backend 由 Application 初始化；Renderer 初始化并关闭 ImGui renderer backend。逆序关闭为：停止 core 上传和 UI 帧 → viewer 销毁全部图片 → renderer 关闭 ImGui renderer backend 和设备/context → platform backend shutdown → ImGui context → 窗口 / SDL。

### 3.3 动画帧上传与呈现

普通 GIF 播放复用现有 create_image / update_image / add_image_quad / present 方法，不新增 renderer 动画会话或计时接口。
Viewer 持有当前显示和候选两张纹理；core tile 只更新候选，收到该帧 COMPLETE、完成上传且单调时钟达到 deadline 后交换句柄。
上一张显示纹理成为下一次候选，完整覆盖后再切换。动画不触发静态图片的 100 ms 部分揭示，避免半帧混合。
两张纹理均计入现有 GPU image budget；放不下时停止播放并报告错误，保留当前完整帧，不先销毁显示帧以强行继续。
CPU 像素仍在 update_image 返回后 release，无需将 core 引用保留到 present。隐藏 / 最小化暂停与循环策略由应用控制器处理。
下一次复用前，上一帧的绘制已经 render_frame / present；OpenGL 命令顺序负责资源更新依赖。未来 DX11 实现须满足相同公共契约。
当前真实 OpenGL 测试回读确认红色首帧和蓝色后续帧均实际显示；统一后端接口没有增加异步上传或 fence 承诺。

## 4. 后端责任

### 4.1 OpenGL 3.3 首版

- `backend/opengl/` 是唯一包含 `SDL_opengl.h`、OpenGL loader 和 `imgui_impl_opengl3.h` 的应用模块。
- SDL Window 创建后，由 renderer 创建并 make-current GL context，设置 swap interval，初始化 ImGui OpenGL3 backend。
- 初始化查询 `GL_MAX_TEXTURE_SIZE` 与 framebuffer 像素尺寸。
- 图片使用 RGBA8 texture；上传适配 core 行步长，保存并恢复会影响纹理上传和 ImGui 的 GL state；需要时将像素复制到 renderer staging。
- 每帧由 renderer 设置 viewport / clear，调用 ImGui OpenGL3 backend，然后通过 SDL 交换缓冲。
- 应用图标、About 图像和截图也通过 renderer API；Application 不调用任何 `gl*`。

### 4.2 D3D11 后续

- `backend/d3d11/` 是唯一包含 Win32、D3D11、DXGI 和 `imgui_impl_dx11.h` 的应用模块。
- Win32 platform adapter 创建 HWND 并传给 renderer；renderer 创建 device / immediate context / swap chain / render target view，初始化 ImGui DX11 backend。
- handle 内部持有 texture 与 SRV；应用和 shell 只保存同一 `ImageHandle`。
- framebuffer resize 时重建 back buffer。设备移除或恢复失败时使旧 generation 失效，viewer 重新提交当前图片。
- device context、纹理更新、ImGui submit、present 固定在 render thread。第一版不静默切换 WARP 软件设备。

两后端差异只出现在 renderer backend：坐标 / UV 适配、纹理对象、状态保存、present、resize 和 device lost。viewer 和 shell 使用 bounds、UV 与 ImageHandle。

## 5. 图片范围和失败行为

首版显示一张 RGBA8 FIT 纹理，解码格式差异留在 C core；不实现 tiled GPU image、区域缓存、原尺寸超大画布、多级纹理或 page table。输出受设备纹理尺寸、renderer GPU image budget 和 framebuffer 可视范围共同约束。100% 仅在 FIT 未缩小源图时启用；细节、缩放到超出 FIT 输出质量和区域分块作为后续工作。

纹理尺寸超限、预算不足、分配失败和非法像素视图必须返回明确 renderer 状态。UI 显示双语错误，同时保留窗口和重新打开操作；目录导航仍待实现。不能回退到把 GL / D3D 原始 ID 传进 Shell。

CPU 与 GPU 预算独立。renderer 不持有 core 像素引用；一次同步上传后即可释放 `lv_event` / `lv_pixels`。后续分块查看使用同一纹理矩形更新 API。

## 6. 线程、资源与测试

- 创建、图像 create/update/destroy、draw command 生成、resize、submit 和 shutdown 全部在 render thread。
- core worker 只向 event queue 发布 CPU 像素，绝不调用 renderer / ImGui。
- ImageHandle 只在创建它的 renderer 存活期间有效；viewer 在 renderer shutdown 前销毁它们。
- 纹理销毁必须发生在 context / device 有效时。关闭顺序见 3.2。
- 窗口最小化可以跳过 present，但 event/core 队列仍按预算处理；恢复后查询新 framebuffer 像素尺寸。
- UI 提示由应用翻译，renderer 返回稳定错误类别及可记录诊断，不接管语言或日志目标。

已测试：非法矩形、短 buffer、行步长、大小溢出、mipmap 字节账本；真实 OpenGL offscreen 契约覆盖带行填充上传、像素颜色回读、旧 handle 拒绝、generation、尺寸超限、回收和 upload state 恢复。五类格式的实际 OpenGL 显示与既有 resize / About 做了截图验证。零 framebuffer、device lost、全部 OOM 路径和跨屏 DPI 尚待验证。

后续 Windows 接入须测试：Win32 platform backend + DX11 renderer backend 分开初始化，Debug / Release、WARP 与硬件设备、resize / DPI / present、device removal、纹理生命周期以及 DXGI debug layer 无 live-object 报告。Linux OpenGL 和 Windows DX11 应运行相同 renderer 公共契约测试；当前尚未验证 Windows。

## 7. 来源

1. Dear ImGui v1.92.0 官方后端文档：<https://github.com/ocornut/imgui/blob/v1.92.0/docs/BACKENDS.md>
2. Microsoft Direct3D 11 `UpdateSubresource`：<https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-updatesubresource>
3. Microsoft DXGI `ResizeBuffers`：<https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-resizebuffers>
