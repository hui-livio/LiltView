# LiltView 目录与 CMake 构建

已有可运行的 SDL3 + OpenGL 3.3 / ImGui 主窗口，C23 核心仍只有初始化函数。图片打开、加载与解码暂不实现，界面相应操作已禁用。

## 1. 当前目录

```text
LiltView/
├── LICENSE                       # Apache License 2.0
├── CMakeLists.txt                 # 顶层配置、选项和子目录
├── CMakePresets.json              # Debug / Release / 兼容模式 / dev
├── .gitmodules                   # submodule 来源与路径声明
├── third_party/
│   ├── README.md                 # submodule / 直接保存头文件的规则
│   └── dependencies.json         # 版本、固定提交与来源清单
├── scripts/init_submodules.py    # 首次注册、固定提交及只读检查
├── cmake/
│   ├── LiltViewOptions.cmake      # 语言特性检查和目标级编译设置
│   └── LiltViewDependencies.cmake # 按需构建 third_party 本地源码
├── core/
│   ├── CMakeLists.txt
│   ├── include/liltview/core.h    # 公共 C API 草案
│   └── src/
│       ├── README.md             # 后续核心模块分区
│       └── api/config.c          # 已实现的初始化和 ABI 版本查询
├── app/
│   ├── CMakeLists.txt
│   ├── README.md                 # 后续应用模块分区
│   ├── main.cpp                  # 命令行与程序入口
│   ├── application.cpp           # 窗口、事件、资源及退出生命周期
│   ├── ui/                      # 菜单、空画布、关于与主题
│   ├── i18n/                    # 中英翻译
│   ├── menu/                    # 系统和窗口菜单共享的菜单树
│   ├── settings/                # 可持久化的窗口、主题与语言设置
│   ├── platform/                # 随包 / 系统字体选择
│   ├── resources/               # 嵌入的应用图标
│   └── logging/
│       ├── core_log.hpp
│       └── core_log.cpp          # spdlog 回调适配，已通过构建
├── tests/
│   ├── CMakeLists.txt
│   ├── contract/                 # 无第三方依赖的语言 / ABI 检查
│   └── core/config_tests.cpp     # Catch2 配置重置与初始化测试
├── benchmarks/README.md          # 后续性能测量
├── packaging/README.md           # 后续 Arch / Windows 打包
├── assets/icons/                 # 已选定的彩色图标
├── assets/fonts/                 # 原始鸿蒙字体、许可及来源校验
├── docs/                         # 产品、接口和工程设计
└── build/                        # 本机生成内容，忽略提交
```

后续核心增加 `engine`、`io`、`codecs`、`cache`、`platform`，应用增加图片 `viewer` 和 `renderer`。更新检测延期，不创建 `update` 源码和网络依赖。

源码清单显式写入各自 CMakeLists，不使用递归 GLOB 自动收集文件，以便清楚审阅新增模块和依赖边界。

Core API 的请求时序、对象所有权、关闭过程、资源预算与边界行为另见 [Core Runtime 契约](CORE_RUNTIME-v0.1.md)。实现尚未完成时，测试覆盖状态以该文档的验收矩阵和 CMake 测试目标为准。

## 2. 构建目标

| 目标 | 当前内容 | 启用条件 |
| --- | --- | --- |
| `LiltView::core` / `liltview_core` | C23 静态库：ABI 版本、配置初始化、请求初始化 | 总是 |
| `liltview_contract_c` | C23、受检算术及实际 C 入口检查 | `BUILD_TESTING=ON` |
| `liltview_contract_cpp20` | C++20 调 C 函数，比较 C / C++ 结构布局 | `BUILD_TESTING=ON` |
| `liltview_contract_cpp23` | C++23 同类检查 | 测试开启且标准选择 23 |
| `LiltView::imgui` | ImGui 核心封装，官方 SDL3 / OpenGL 后端单独编译 | 应用组件开启 |
| `LiltView::ui` | UI 静态库：菜单、空画布、主题及关于 | 应用组件开启 |
| `LiltView::app_model` | 翻译、菜单、设置解析及安全文件替换，无窗口依赖 | 应用组件开启 |
| `liltview_fonts` | 字体候选、资源校验和 ImGui 加载 | 应用组件开启 |
| `LiltView::logging` | spdlog 日志回调静态库 | 应用组件开启 |
| `liltview_core_tests` | Catch2 单元测试，CTest 自动发现 | 单元测试开启 |
| `liltview_app_tests` | 翻译和设置 Catch2 测试 | 应用组件及单元测试开启 |
| `liltview` | 主窗口可执行文件 | 应用组件开启 |

只有 `lv_core_abi_version`、`lv_core_config_init`、`lv_request_desc_init` 已实现，其余接口在调用时仍无法链接。没有为未实现的引擎函数提供伪成功桩。

## 3. 工具链与默认构建

要求 CMake 3.30+、Ninja、满足项目特性检查的 C23 编译器和 C++23 编译器。配置期检查 C23 的 `constexpr`、`nullptr`、`stdckdint.h`，默认 C++23 检查 `std::expected`。这些是本工程基线，不代表完整标准符合性认证。

```bash
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

默认无第三方依赖、不发起依赖下载，`BUILD_TESTING=ON`，仅运行独立契约检查。

```bash
cmake --preset release
cmake --build --preset release
```

`release` 是不含窗口的核心构建，默认不构建测试；产物为 `build/release/core/libliltview_core.a`。主程序使用下面的 `dev` 或 `app-release`。

显式选择 C++20 兼容模式：

```bash
cmake --preset compat20
cmake --build --preset compat20
ctest --preset compat20
```

C 核心始终为 C23。所有项目自身编译目标关闭 GNU 语言扩展，可用 `-DLILTVIEW_WARNINGS_AS_ERRORS=ON` 将警告视为错误。该设置不施加给第三方源码。

自有 C++ 目标未使用模块，显式关闭 `CXX_SCAN_FOR_MODULES`，避免不必要的 GCC / Ninja 动态模块扫描和切换 20 / 23 时的 dyndep 问题。

不使用 presets 时也可以：

```bash
cmake -S . -B build/custom -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/custom
ctest --test-dir build/custom --output-on-failure
```

## 4. 第三方依赖

### 4.1 源码管理

需要编译的库用 Git submodule，纯头文件库直接保存源码，全部放在 `third_party/`。当前 ImGui、spdlog、SDL3、Catch2 四个库均使用 submodule。

基线为 ImGui v1.92.0、spdlog v1.15.3、SDL3 release-3.4.0、Catch2 v3.8.1，具体提交和发布来源记录在 [依赖清单](../third_party/dependencies.json)。主仓库 gitlink 是实际 submodule 版本锁；manifest 与它应一致。

四个 submodule 已注册，源码已就绪，gitlink 与清单校验通过。首次引入或维护时执行：

```bash
python3 scripts/init_submodules.py
python3 scripts/init_submodules.py --check
git diff --cached --submodule
```

脚本只暂存 submodule 声明和 gitlink，不提交或推送；首次注册后须将 gitlink 与相关 CMake / 清单一起提交。仅提交 `.gitmodules` 不能完成接入。详细规则见 [third_party 说明](../third_party/README.md)。

### 4.2 正常克隆与构建

以下命令适用于主仓库已记录 gitlink 的后续克隆：

```bash
git submodule update --init --recursive
python3 scripts/init_submodules.py --check
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

`dev` 开启应用组件和 Catch2 测试。也可分别设置 `LILTVIEW_BUILD_APP_COMPONENTS` 和 `LILTVIEW_BUILD_UNIT_TESTS`，后者要求 `BUILD_TESTING=ON`。默认 debug / release / compat20 仍不需要第三方源码。

移除原先的 SYSTEM / FETCH 模式和对应 presets。CMake 只读本地源码，不运行 Git、不联网；缺少依赖时提示如何初始化。源码准备好后，配置和构建可以离线运行。

### 4.3 纯头文件库

直接提交到 `third_party/<name>/`，附上原许可证、上游提交和文件校验信息，使用 INTERFACE target 暴露路径。当前没有额外引入纯头文件库；cpp-httplib 延期，不为满足目录布局提前下载它。

日志回调使用 spdlog 的默认 logger，项目可在应用入口配置 sink 和等级。适配器按显式长度处理文本，并在 C 边界捕获异常。ImGui 使用锁定版本中的 SDL3 / OpenGL 官方后端，不改动第三方源码。

cpp-httplib、TLS 库和更新检测均不在当前依赖图内。

## 5. 本次验证

- 当前 GCC/G++ 16.2.1、CMake 4.4.5、Ninja 1.13.2 下：Debug 构建通过，CTest 3/3；显式 C++20 构建通过，CTest 2/2；Release 静态库构建通过。
- Debug / C++20 使用 warnings-as-errors。
- 契约检查覆盖跨 C / C++ 的 14 个公开结构大小与对齐、关键字段偏移，以及实际 C 编译初始化函数的链接调用。
- `dev` 已完成主程序、实际依赖和应用组件构建，CTest 12/12 通过；缺少源码时明确提示，不自动联网。
- 首次注册脚本以临时本地 Git 仓库验证：预声明 `.gitmodules` 后注册、检出固定旧提交、重复执行、只读检查及拒绝覆盖脏工作区。此检查不代表真实第三方库已经构建。
- 四个真实 gitlink 与本地检出已通过 `scripts/init_submodules.py --check`。
- Wayland、X11、SDL offscreen 三种路径均完成窗口创建、渲染、调整尺寸、关于弹窗、关闭事件的冒烟测试；中英文截图已检查。
- 普通事件循环退出和设置文件保存已验证；从 `/tmp` 启动仍可加载随包字体。Windows 尚未验证，IME 和跨屏高 DPI 需要后续人工验收。
- 主程序既有的 C++20 兼容、Release 安装迁移验证已完成；本轮已将 SDL 改为静态链接，最新项目级修复与测试数量见 [项目审查](PROJECT_REVIEW-2026-11-07.md)，不能以旧动态链接安装结果代替验证。

## 6. 下一步

窗口与渲染已采用 SDL3 / OpenGL，下一步选择解码库并实现 C 引擎。测试随任务、事件、内存和解码行为扩展；当前图片操作仍禁用。

## 7. 主程序运行与资源

```bash
cmake --preset dev
cmake --build --preset dev
./build/dev/app/liltview --language zh-CN
```

菜单提供语言、主题、全屏和关于。主题有跟随系统 / 深色 / 浅色，默认跟随系统；启动读取 SDL_GetSystemTheme，并在 SDL_EVENT_SYSTEM_THEME_CHANGED 时更新。显式选择始终优先，未获得系统偏好时采用深色。配置分别保存 `theme=system|dark|light`，兼容旧深浅配置；菜单勾选的是偏好模式，不把“跟随系统下恰好为深色”当作手动深色。

在 KDE Plasma 上优先导出到系统顶栏，其他桌面回退到窗口内部，窗口标题栏仍由桌面管理。快捷键：F11 切换全屏，Esc 退出全屏 / 关闭关于，Ctrl+Q 退出。静止窗口通过 SDL 事件等待降低重绘频率。配置保存在 SDL 用户应用目录下的 `settings.ini`，可用 `--config-dir PATH` 隔离测试。

默认字体为原始 HarmonyOS Sans SC Regular，位于 `assets/fonts/`，构建时复制到可执行文件相邻资源目录。详情与独立许可见 [字体说明](../assets/fonts/README.md)。“关于”提供字体署名。缺失随包字体时尝试系统字体，无中文字体时使用英文。

```bash
cmake --preset app-release
cmake --build --preset app-release
cmake --install build/app-release --prefix "$PWD/build/package"
./build/package/bin/liltview
```

安装包含静态链接裁剪版 SDL3 的主程序、字体和许可证，不安装 SDL `.so`，不添加 SDL 专用 RPATH。系统 OpenGL / Fontconfig / GIO（全局菜单开启时）及窗口运行库仍需存在。默认 `release` 依旧是核心库构建。

无需显示服务器的冒烟测试（要求 EGL / OpenGL offscreen 驱动）：

```bash
SDL_VIDEODRIVER=offscreen ./build/dev/app/liltview --smoke-test --language zh-CN --screenshot build/dev/window.ppm
```

此模式创建真实 GL 上下文，调整窗口，打开 / 关闭关于，经过渲染循环再通过 SDL 关闭事件退出；不显示窗口、不写用户设置。截图输出 PPM 主画面及 `window-about.ppm`。也可将驱动改为 `x11` 或 `wayland` 验证对应本机后端。`--help` 和 `--version` 不初始化图形环境。

## 8. 系统菜单、Dock 与事件循环

Linux 默认 `LILTVIEW_ENABLE_GLOBAL_MENU=ON`。需要系统 GIO / GLib、Wayland 客户端、X11 开发库和 wayland-scanner，不引入 Qt 或 libdbusmenu。菜单通过 D-Bus `com.canonical.dbusmenu` 导出，Wayland 通过 KDE appmenu 协议关联 wl_surface，X11 通过 registrar 和窗口属性关联。菜单线程仅发送动作和 SDL 唤醒事件，主线程统一处理设置 / 全屏 / 关于 / 退出。

菜单由同一 `MenuItem` 树提供稳定 ID、翻译、禁用状态、勾选和快捷键。桌面未提供对应全局菜单服务或窗口无法关联时，窗口内保留可操作菜单。桌面需要有全局菜单组件；程序不自动修改用户系统面板。可显式关闭该集成：

```bash
cmake --preset dev -DLILTVIEW_ENABLE_GLOBAL_MENU=OFF
```

`Application::run()` 只保留初始化、局部菜单生命周期、主循环及设置保存。详细职责分为 `create_window`、`initialize_imgui`、`load_font`、`process_events`、`handle_event`、`handle_shortcut`、`synchronize_menu`、`render_frame`、`present_frame` 等，阶段注释说明主线程限制和资源释放顺序。

SDL 初始化前设置应用 ID `io.github.hui_livio.LiltView`，供 Wayland app_id / X11 WM_CLASS 使用；CMake 安装同名 desktop 和图标。直接运行开发构建前，用户级注册为：

```bash
python3 scripts/register_desktop.py
```

脚本写入当前用户 XDG applications 和 hicolor 图标路径，指向默认 `build/dev/app/liltview`；其他构建可用 `--executable PATH` 指定。不会改变 Dock 排列或固定项。当前用户已注册默认开发构建，需要重新启动旧进程后识别新的 app_id。

主界面原先的 64px 图标显示约 82 个逻辑像素，会放大而模糊。现采用 256px 嵌入源、mipmap 缩小采样和 framebuffer 像素对齐，覆盖常见 2x / 3x 显示。资源生成脚本为 `scripts/generate_icon.py`，仅重新生成资源时需要 ImageMagick，正常构建不需要它。

本轮验证：17 项测试通过，包含三态主题解析、持久化、跟随优先级及互斥勾选；真实 D-Bus GetLayout / GetGroupProperties / Event / EventGroup / AboutToShowGroup、语言 / 主题 / 关于 / 退出动作及保存通过；Wayland 协议日志确认 app_id 和 appmenu 地址已送达合成器。Windows 全局菜单无此实现，使用窗口内菜单。

上面的 17 项是主窗口阶段的历史结果；资源异常与设置文件回归加入后，开发配置为 23 项，启用 `LILTVIEW_BUILD_DESKTOP_TESTS=ON` 后为 26 项。后者要求 EGL / OpenGL offscreen、Python、gdbus、dbus-run-session，测试使用私有总线和临时目录。详见 [项目级审查报告](PROJECT_REVIEW-2026-11-07.md)。

函数职责已集中审查，UI 区域、D-Bus 方法和生命周期、平台字体、菜单构造、参数解析和维护脚本均作了拆分；详见 [可读性审查记录](CODE_REVIEW-2026-11-07.md)。About 新增作者 hui-livio、邮箱 yuanfeng1897@gmail.com 和可点击的项目地址。
