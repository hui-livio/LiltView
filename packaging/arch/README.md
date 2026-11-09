# Arch Linux 打包与部署

提供本地源码包、PKGBUILD 和 `.SRCINFO` 生成脚本。支持 makepkg / pacman，以及 paru 的本地 PKGBUILD 模式；当前没有发布 AUR 包，不使用 `paru -S liltview` 作为安装入口。

## 1. 准备环境

在 Arch Linux x86_64 上，以普通用户执行。首次准备基础工具：

```sh
sudo pacman -S --needed base-devel git python
git submodule update --init --recursive
```

构建还需要 CMake 3.30+、Ninja、pkgconf、支持 C23 的 GCC、NASM、系统 OpenGL / Fontconfig / GIO 及 X11 / Wayland 库。完整构建、运行及检查依赖由生成的 PKGBUILD 声明；`--syncdeps` 或 paru 可以交给 pacman 安装缺失依赖，保留正常交互确认。

主程序使用 C23 / C++23、Release / NDEBUG 和 makepkg 的优化及加固标志，禁用额外 debug 子包。x86_64 把 NASM 列为构建依赖，供 libjpeg-turbo 启用 SIMD。SDL、图片 codec、ImGui、spdlog 静态链接；窗口系统和图形驱动仍由系统提供。普通安装无需系统 SDL3，也不覆盖系统库。

## 2. pacman 路径

在仓库根目录运行：

```sh
# 只构建 .pkg.tar.zst；--syncdeps 允许安装缺失构建依赖。
python3 scripts/package_arch.py build --syncdeps --jobs 4

# 或构建后直接安装，由 makepkg 调用 pacman。
python3 scripts/package_arch.py install --manager pacman --syncdeps --jobs 4
```

构建结束打印准确的软件包路径；也可手动执行：

```sh
sudo pacman -U /absolute/path/to/liltview-VERSION-1-x86_64.pkg.tar.zst
```

`build` 默认不修改已安装软件；指定 `--syncdeps` 才允许 makepkg 安装缺失依赖。`install` 明确请求安装应用包，升级已安装本地版本也使用同一命令。不要用 `sudo` 执行整个脚本或 makepkg。

## 3. paru 路径

已有 paru 时，直接从当前工作区构建并安装：

```sh
python3 scripts/package_arch.py install --manager paru --jobs 4
```

脚本实际调用 `paru --build --install <生成目录>`（即 `paru -Bi <目录>`），依赖解析和安装由 paru / pacman 处理。仅构建使用 `build --manager paru`。paru 不需要额外传 `--syncdeps`。

也可以先准备素材，再直接调用 paru：

```sh
python3 scripts/package_arch.py prepare
# 使用上一步输出的准确目录：
paru -Bi /absolute/path/to/generated-package-directory
```

以上是本地 PKGBUILD 工作流，不能因此通过 `paru -Syu` 自动发现 GitHub 新提交。后续发布 AUR / 自建仓库时另加发布流程。

## 4. 产物与源码追踪

默认输出到 `build/arch/<版本>-<pkgrel>-<源码校验前缀>/`，包含：

- `PKGBUILD`、`.SRCINFO`：可以独立审阅、复制到另一台 Arch 机器构建。
- `liltview-<版本>.tar.gz`：包含当前源码、字体、许可证及递归 submodule 的实际源码，构建时不再联网下载第三方库。
- `liltview-<版本>-<pkgrel>-x86_64.pkg.tar.zst`：构建成功后可交给 pacman 安装；包压缩后缀遵循本机 makepkg.conf。
- `src/`、`pkg/`：makepkg 的源码和安装暂存目录。

版本为 `0.1.0.r<提交数>.g<提交哈希>`；未提交改动时追加 `.dirty`。应用内版本仍为 CMake 的基础版本。源码归档附 `LILTVIEW_SOURCE.json` 记录主仓库与 submodule 提交；PKGBUILD 使用真实 SHA-256，不跳过完整性校验。

归档包含 Git 跟踪文件的当前内容及未被忽略的新文件，不包含 `.git`、Git 忽略的本地文件或输出目录。缺失 submodule 或 checkout 与 gitlink 不符时先报错，不在打包过程中重置依赖。请在源码工作区稳定时打包；发布前提交源码可得到可追溯的干净版本。

`--output-dir PATH` 可改变产物根目录，`--pkgrel N` 可增加同版本打包修订。脚本生成目录中的 PKGBUILD 是产物，修改模板使用 `packaging/arch/PKGBUILD.in`；模板含占位符，不能直接作为 PKGBUILD 执行。

## 5. 安装内容与卸载

| 路径 | 内容 |
| --- | --- |
| `/usr/bin/liltview` | 主程序 |
| `/usr/share/liltview/fonts/` | 原始鸿蒙字体、字体许可证和来源记录 |
| `/usr/share/applications/io.github.hui_livio.LiltView.desktop` | 桌面入口及图片 MIME 声明 |
| `/usr/share/icons/hicolor/` | 彩色 SVG / 多尺寸 PNG 图标 |
| `/usr/share/licenses/liltview/` | Apache 许可证、NOTICE 和全部随包依赖的许可 |

图标和桌面缓存由系统现有 pacman hooks 处理；不另外写 post_install 脚本，不改默认图片关联、不注册指向开发目录的用户启动器。正式安装不需要运行 `scripts/register_desktop.py`。如果以前手动注册过开发启动器，用户级同名入口可能优先于系统入口，需根据自己的使用方式调整。

启动和卸载：

```sh
liltview
liltview /path/to/photo.jpg
sudo pacman -Rns liltview
# 或：paru -Rns liltview
```

用户配置仍按原有 SDL 用户目录保存，不随卸载删除。顶栏菜单依赖桌面已有全局菜单组件；没有时应用使用窗口内菜单。

## 6. 检查

```sh
python3 scripts/package_arch.py prepare
# 在输出目录中：
makepkg --printsrcinfo
makepkg --verifysource
makepkg --syncdeps
pacman -Qip ./liltview-*.pkg.tar.zst
pacman -Qlp ./liltview-*.pkg.tar.zst
```

PKGBUILD 的 `check()` 验证实际可执行文件版本和 desktop 文件语法；不要求桌面会话。发布前还应在干净 Arch 构建环境检查依赖，在有桌面的机器验证 X11 / Wayland 和图形驱动。安装需要管理权限，打包、归档与离屏冒烟测试不需要安装应用到系统。

操作语义以本机 `makepkg(8)`、`PKGBUILD(5)`、`pacman(8)` 和 `paru(8)` 为准。性能待办见[开发计划](../../docs/DEVELOPMENT_PLAN.md)，打包能力不代表加载速度问题已解决。

## 7. 历史打包验证（2026-11-09）

- 开发版 105 项常规测试通过；实际 makepkg 完成源码 SHA-256 校验、Release 构建、`check()`、打包和元数据生成，JPEG 的 NASM SIMD 对象参与构建。
- 实测本地包压缩约 7.59 MiB、安装约 13.34 MiB；体积随工具链、makepkg 标志和压缩配置变化。
- pacman 能读取包元数据；检查程序、字体、桌面文件、图标及许可证齐全，无 SDL / codec 动态库、RPATH 或本机构建路径残留。解包后的程序在真实 SDL / OpenGL 离屏路径显示 PNG / JPEG；安装前缀移动到含空格路径后也能加载随包字体。
- 两次相同源码归档的 SHA-256 一致，确认递归依赖源码被包含，Git 元数据、Python 缓存和根目录构建产物被排除。脚本的 makepkg 与 paru 分支使用已构建目录复打包验证通过。

验证没有安装应用到系统，也没有执行需要管理权限的 pacman 安装事务。本机 NASM 通过已校验仓库 SHA-256 的软件包解压到局部工具目录提供；其余声明依赖由 `pacman -T` 检查，因此仅在这次验证命令中跳过依赖解析。正式脚本和 PKGBUILD 不跳过依赖检查，安装事务及干净 chroot 构建仍待实际环境验证。
