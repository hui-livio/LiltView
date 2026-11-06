# LiltView 目录与 CMake 构建

当前已初始化 CMake 骨架。可离线编译 C23 核心初始化函数并执行 C / C++ 契约检查；尚无窗口可执行程序或图片解码引擎。

## 1. 当前目录

```text
LiltView/
├── LICENSE                       # Apache License 2.0
├── CMakeLists.txt                 # 顶层配置、选项和子目录
├── CMakePresets.json              # Debug / Release / 兼容模式 / 依赖模式
├── cmake/
│   ├── LiltViewOptions.cmake      # 语言特性检查和目标级编译设置
│   └── LiltViewDependencies.cmake # 按需查找或获取第三方依赖
├── core/
│   ├── CMakeLists.txt
│   ├── include/liltview/core.h    # 公共 C API 草案
│   └── src/
│       ├── README.md             # 后续核心模块分区
│       └── api/config.c          # 已实现的初始化和 ABI 版本查询
├── app/
│   ├── CMakeLists.txt
│   ├── README.md                 # 后续应用模块分区
│   └── logging/
│       ├── core_log.hpp
│       └── core_log.cpp          # spdlog 回调适配，待第三方构建验证
├── tests/
│   ├── CMakeLists.txt
│   ├── contract/                 # 无第三方依赖的语言 / ABI 检查
│   └── core/config_tests.cpp     # Catch2 配置重置与初始化测试
├── benchmarks/README.md          # 后续性能测量
├── packaging/README.md           # 后续 Arch / Windows 打包
├── assets/icons/                 # 已选定的彩色图标
├── docs/                         # 产品、接口和工程设计
└── build/                        # 本机生成内容，忽略提交
```

后续目录按实际源码逐步建立：核心增加 `engine`、`io`、`codecs`、`cache`、`platform`；应用增加 `ui`、`viewer`、`renderer`、`platform`、`i18n`。更新检测延期，不创建 `update` 源码和网络依赖。

源码清单显式写入各自 CMakeLists，不使用递归 GLOB 自动收集文件，以便清楚审阅新增模块和依赖边界。

## 2. 构建目标

| 目标 | 当前内容 | 启用条件 |
| --- | --- | --- |
| `LiltView::core` / `liltview_core` | C23 静态库：ABI 版本、配置初始化、请求初始化 | 总是 |
| `liltview_contract_c` | C23、受检算术及实际 C 入口检查 | `BUILD_TESTING=ON` |
| `liltview_contract_cpp20` | C++20 调 C 函数，比较 C / C++ 结构布局 | `BUILD_TESTING=ON` |
| `liltview_contract_cpp23` | C++23 同类检查 | 测试开启且标准选择 23 |
| `LiltView::imgui` | ImGui 依赖封装，未选择窗口 / 渲染后端 | 应用组件开启 |
| `LiltView::ui` | 链接核心与 ImGui 的 INTERFACE 目标，没有 UI 源码 | 应用组件开启 |
| `LiltView::logging` | spdlog 日志回调静态库 | 应用组件开启 |
| `liltview_core_tests` | Catch2 单元测试，CTest 自动发现 | 单元测试开启 |

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

Release 默认不构建测试。当前产物是 `build/release/core/libliltview_core.a`（Windows 下文件名依工具链变化），不是看图程序，也没有应用安装目标。

显式选择 C++20 兼容模式：

```bash
cmake --preset compat20
cmake --build --preset compat20
ctest --preset compat20
```

C 核心始终为 C23。所有项目自身编译目标关闭 GNU 语言扩展，可用 `-DLILTVIEW_WARNINGS_AS_ERRORS=ON` 将警告视为错误。该设置不施加给第三方源码。

不使用 presets 时也可以：

```bash
cmake -S . -B build/custom -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build/custom
ctest --test-dir build/custom --output-on-failure
```

## 4. 第三方依赖

### 4.1 显式启用

```bash
# 要求本机安装可发现的 ImGui、spdlog、Catch2 开发包。
cmake --preset dev-system
cmake --build --preset dev-system
ctest --preset dev-system

# 或显式允许 FetchContent 获取版本化源码，需要 Git 与可用网络。
cmake --preset dev-fetch
cmake --build --preset dev-fetch
ctest --preset dev-fetch
```

两者都会开启应用组件和 Catch2 测试。也可分别设置 `LILTVIEW_BUILD_APP_COMPONENTS` 和 `LILTVIEW_BUILD_UNIT_TESTS`。单元测试选项要求 `BUILD_TESTING=ON`。

SYSTEM 模式通过包配置查找 spdlog、Catch2；ImGui 优先配置包的 `imgui::imgui`，再尝试 pkg-config。系统包需提供 ImGui 1.92+、spdlog 1.15+、Catch2 3.8+；不满足时明确失败，不静默转 FETCH。

FETCH 的首轮版本基线是 ImGui `v1.92.0`、spdlog `v1.15.3`、Catch2 `v3.8.1`，定义在 `cmake/LiltViewDependencies.cmake`；它们是版本化引导引用，不表示“最新版本”，也不是不可变提交锁。当前网络不可用，尚未核实下载与集成构建，正式启用时需确认版本可用性、记录验证结果，并在发布前改成已核验的提交 SHA 或源码包哈希。

FETCH 可使用 CMake 的 `FETCHCONTENT_SOURCE_DIR_IMGUI`、`FETCHCONTENT_SOURCE_DIR_SPDLOG`、`FETCHCONTENT_SOURCE_DIR_CATCH2` 指向事先准备好的对应版本源码，用于离线集成；本次没有通过假第三方头或函数桩代替真实依赖验证。

日志回调使用 spdlog 的默认 logger，项目可在应用入口配置 sink 和等级。适配器按显式长度处理文本，并在 C 边界捕获异常。ImGui target 暂只包含核心源文件，窗口与渲染后端在选型后再加入。

cpp-httplib、TLS 库和更新检测均不在当前依赖图内。

## 5. 本次验证

- 当前 GCC/G++ 16.2.1、CMake 4.4.5、Ninja 1.13.2 下：Debug 构建通过，CTest 3/3；显式 C++20 构建通过，CTest 2/2；Release 静态库构建通过。
- Debug / C++20 使用 warnings-as-errors。
- 契约检查覆盖跨 C / C++ 的 14 个公开结构大小与对齐、关键字段偏移，以及实际 C 编译初始化函数的链接调用。
- `dev-system` 在未安装 ImGui 时明确配置失败；未偷偷下载依赖。
- 当前环境无法解析 GitHub 域名，未完成 FETCH 路径、spdlog 适配器或 Catch2 用例的实际构建；这些不计为已通过测试。

## 6. 下一步

选定窗口 / 渲染后端与解码库，完成真实第三方依赖构建后，添加应用入口及 C 引擎实现。测试随任务、事件、内存和解码行为逐步扩展；安装打包在实际可执行程序形成后补充。
