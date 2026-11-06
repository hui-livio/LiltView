# 第三方源码管理

统一放在 `third_party/`，按构建形式分为两类：

- **需要编译的库**：Git submodule，主仓库的 gitlink 固定具体提交。
- **纯头文件库**：直接提交所需头文件，保留原许可证、上游版本 / 提交与来源记录，不额外创建 submodule。

spdlog 本项目采用编译库形式；因此与 ImGui、SDL3、Catch2 一样使用 submodule。后续 cpp-httplib 为直接保存头文件的候选，但更新模块延期，本轮不下载或构建它。字体是资源，保存在 `assets/fonts/` 并附原许可和校验信息。

## 当前声明

| 路径 | 版本基线 | 固定提交 | 接入形式 |
| --- | --- | --- | --- |
| `imgui/` | v1.92.0 | `85b2fe8486190fa9326565a2fb5fccb6caea4396` | submodule |
| `spdlog/` | v1.15.3 | `6fa36017cfd5731d617e1a934f0e5ea9c4445b13` | submodule、编译库 |
| `Catch2/` | v3.8.1 | `2b60af89e23d28eefc081bc930831ee9d45ea58b` | submodule、仅测试 |
| `SDL/` | release-3.4.0 | `a962f40bbba175e9716557a25d5d7965f134a3d3` | submodule、窗口 / 输入 |

初始化脚本读取 `dependencies.json`，其发布页字段记录版本来源。基线不是“最新版本”声明，升级时需同时审阅 manifest 和 gitlink 改动。

## 首次注册

四个 submodule 已注册并取得源码，gitlink 与 `dependencies.json` 的固定提交一致，`--check` 校验通过。SDL3 只构建动态库，关闭音频、相机、手柄、传感器等未用子系统；保留窗口、输入、OpenGL 以及 Linux 的 X11 / Wayland。系统 OpenGL、Fontconfig 和窗口系统开发库不作为本项目源码 vendoring 的对象。

`plasma-wayland-protocols/` 保存 KDE appmenu 协议描述、原 MIT 许可和带 SHA-256 的来源清单。它是构建时生成 Wayland C 接口所需的协议数据，不是完整第三方库；没有复制 Qt 实现或链接 Qt。生成工具 wayland-scanner、GIO / GLib、Wayland 和 X11 由系统开发环境提供。

下列首次注册流程用于维护或重新建立依赖。仅有 `.gitmodules` 不等于 submodule 已完成接入，必须同时记录 gitlink。

在具备网络和可写 Git 元数据的终端执行：

```bash
python3 scripts/init_submodules.py
python3 scripts/init_submodules.py --check
git diff --cached --submodule
```

脚本会注册库、检出固定提交并暂存 `.gitmodules` 与 gitlink，不自动提交或推送。遇到依赖目录已有普通源码或子模块工作区有修改时停止，不覆盖它们。注册失败可能已完成前面的库；解决错误后可以重跑。

首次完成后，应将 gitlink 与 CMake / 文档 / 清单一并提交。之后其他开发者正常使用：

```bash
git submodule update --init --recursive
python3 scripts/init_submodules.py --check
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

通常不使用 `git submodule update --remote`，以免绕过主仓库锁定版本。CMake 只读取本地源码，不自动执行 Git、下载或修改依赖。

## 纯头文件库规则

直接保存到 `third_party/<name>/`，至少包含实际头文件、原始 LICENSE 和来源记录。后续在 manifest 的 `vendored_headers` 中登记版本、上游提交和保存文件的 SHA-256。若有额外实现 `.c/.cpp` 或要求某个宏生成实现，则按实际链接方式审查，不仅凭“单文件”归类。

头文件库在 CMake 中用 INTERFACE target 暴露包含路径；上游目录标记为 SYSTEM，避免将项目自身的严格告警施加给第三方代码。第三方文件遵循原许可证，不改标为本项目 Apache-2.0。
