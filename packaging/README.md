# 打包

当前提供主窗口的 CMake 安装规则，尚未生成 Arch 软件包或 Windows 安装器：

```bash
cmake --preset app-release
cmake --build --preset app-release
cmake --install build/app-release --prefix "$PWD/build/package"
./build/package/bin/liltview
```

安装目录包含主程序、构建的 SDL3 动态库、鸿蒙字体与其原始许可证，以及项目和运行时第三方许可证。Linux 安装后的程序通过相对 RPATH 寻找 SDL3；仍依赖系统 OpenGL、Fontconfig 及窗口系统运行库。当前只有应用壳层，不能打开图片。

Linux 同时安装 `io.github.hui_livio.LiltView.desktop` 与同名 hicolor SVG / PNG，和程序的 app_id / WM_CLASS 一致，供 Dock / 任务栏匹配。安装到非标准 prefix 后，需让该 prefix 的 `share` 进入 XDG_DATA_DIRS，或使用 `scripts/register_desktop.py --executable <prefix>/bin/liltview` 注册用户入口。

全局菜单默认依赖系统 GIO、Wayland、X11；安装附 MIT 许可的 KDE appmenu 协议归属文件。系统顶栏需要桌面已提供全局菜单组件，不存在时程序回退窗口内菜单。

应用图标源使用 `assets/icons/liltview-color.svg` 及对应 PNG；Windows ICO 已保留，原生资源与安装器接入留待 Windows 验证。鸿蒙字体保持原样并随应用分发，不单独作为产品分发。
