# 打包

已提供 [Arch Linux 打包部署脚本](arch/README.md)，支持 makepkg / pacman 与 paru 本地 PKGBUILD 安装。Windows 安装器仍待实现。Linux 只安装到自定义目录时也可沿用 CMake：

```bash
cmake --preset app-release
cmake --build --preset app-release
cmake --install build/app-release --prefix "$PWD/build/package"
./build/package/bin/liltview
```

Windows 在 x64 Developer Shell 中构建 `msvc-release` 后，也可用 `cmake --install build/msvc-release --prefix "D:/LiltView-package"` 暂存程序、字体和许可证；入口为 `<prefix>/bin/liltview.exe`。这是 CMake 安装布局，不是 Windows 安装器，干净 Windows 环境部署仍需独立验收。

安装目录包含静态链接裁剪版 SDL 的主程序、`share/liltview/fonts/` 下的鸿蒙字体与其原始许可证，以及项目和第三方许可证。构建目录继续使用可执行文件旁的 `assets/fonts/`，安装后的资源按 bin / share 的相对布局查找，可整体移动安装前缀。**不安装 SDL 动态库或设置 SDL 专用 RPATH**，不会覆盖发行版的 SDL；Linux 仍依赖系统 OpenGL、Fontconfig、GIO（启用全局菜单时）和窗口系统运行库；Windows 使用系统图形 / Win32 运行库，不依赖 Linux Fontconfig / GIO。支持常规 JPEG；libjpeg-turbo 静态链接并附 LICENSE.md / README.ijg，不覆盖系统 JPEG 库。

验证安装时使用新的 staging 目录：旧动态链接构建留下的 `.so` 文件不会由 CMake 自动删除，不能将旧 staging 文件误认为新的安装产物。可用 `readelf -d <prefix>/bin/liltview` / `ldd` 检查动态依赖中没有 SDL。

Linux 同时安装 `io.github.hui_livio.LiltView.desktop` 与同名 hicolor SVG / PNG，和程序的 app_id / WM_CLASS 一致，供 Dock / 任务栏匹配。安装到非标准 prefix 后，需让该 prefix 的 `share` 进入 XDG_DATA_DIRS，或使用 `scripts/register_desktop.py --executable <prefix>/bin/liltview` 注册用户入口。

全局菜单默认依赖系统 GIO、Wayland、X11；安装附 MIT 许可的 KDE appmenu 协议归属文件。系统顶栏需要桌面已提供全局菜单组件，不存在时程序回退窗口内菜单。

应用图标源使用 `assets/icons/liltview-color.svg` 及对应 PNG；Windows 已通过 `app/resources/liltview.rc.in` 接入多尺寸 ICO 与文件 / 产品版本资源，Release 使用 GUI 子系统。Windows 安装器及完整部署验收仍待实现。鸿蒙字体保持原样并随应用分发，不单独作为产品分发。

常用 Arch 命令（仓库根目录，以普通用户执行）：

```sh
python3 scripts/package_arch.py build --syncdeps --jobs 4
python3 scripts/package_arch.py install --manager pacman --syncdeps --jobs 4
# 或使用已安装的 paru：
python3 scripts/package_arch.py install --manager paru --jobs 4
```

生成的 `.pkg.tar.zst` 可用 `sudo pacman -U <准确文件路径>` 安装。本地源码包已经包含 submodule 源码，不要求构建机器访问 GitHub；系统依赖仍由 pacman 解析。尚未发布 AUR，不以 `paru -S liltview` 作为当前入口。
