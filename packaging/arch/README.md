# Arch Linux 编译、打包与部署

`PKGBUILD` 是可直接交给 makepkg 的原生脚本，**不是模板，不需要先运行 Python 生成**。
它从当前完整项目源码编译，适用于 Git checkout 和解压后的完整 Release 源码包，不要求 `.git`。
仅拿走一个 PKGBUILD 文件、没有项目源码时不能构建；这不是从网络获取源码的 AUR 配方。

## 直接使用 makepkg / pacman

在 Arch Linux x86_64 上以普通用户运行：

```sh
sudo pacman -S --needed base-devel git
# Git checkout 需要初始化完整第三方依赖；Release 完整源码包无需此步。
git submodule update --init --recursive
cd packaging/arch
makepkg -s       # 安装缺失构建依赖，不编译/运行 UT，生成 .pkg.tar.zst
# 或者一次完成编译并安装：
makepkg -si
```

makepkg 负责从源码编译，pacman 负责安装及解析系统依赖。不要用 sudo 运行 makepkg。
安装已构建的包：

```sh
sudo pacman -U ./liltview-1.0.0rc.1-1-x86_64.pkg.tar.zst
```

依赖、工具链要求、桌面验证由 PKGBUILD 声明。普通安装默认设置
`BUILD_TESTING=OFF` 和 `LILTVIEW_BUILD_UNIT_TESTS=OFF`，**既不编译也不运行 UT**。
`check()` 仅检查应用版本与 desktop 文件，不运行 CTest。主程序及核心按
**MinSizeRel / NDEBUG** 编译；makepkg 执行 strip，不产生 debug 子包，不安装 UT。SDL 和图片 codec 静态链接，窗口库、Fontconfig、GIO
和图形驱动由系统提供，不要求系统安装 SDL3。许可证直接安装到
`/usr/share/licenses/liltview`，不执行仅大小写不同的目录重命名，兼容 WSL Windows 挂载盘。

## 显式运行 UT（开发 / CI）

```sh
LILTVIEW_RUN_TESTS=1 makepkg --check -sf
```

这会开启 `BUILD_TESTING` 与 `LILTVIEW_BUILD_UNIT_TESTS`，构建并运行完整 **MinSizeRel UT**；
测试失败仍会中止打包，不忽略失败。`--check` 确保即使本机 makepkg 配置禁用了检查也会执行。
不设置该环境变量（或设为 `0`）就是普通安装模式；复用旧构建目录时也会重新明确关闭 UT。
Python 入口对应参数是 `--tests`。

## 预发布与正式版本

源码 checkout 默认打包 `1.0.0rc.1`。完整发布源码包带有 `RELEASE_VERSION`，自动选择
对应 RC / 正式版本。CI 也可通过 `LILTVIEW_RELEASE_VERSION` 明确选择：

```sh
LILTVIEW_RELEASE_VERSION=1.0.0-rc.2 makepkg -sf
# RC 验收完成后：
LILTVIEW_RELEASE_VERSION=1.0.0 makepkg -sf
```

Arch pkgver 使用 `1.0.0rc.1`，应用显示 `1.0.0-rc.1`；正式包 `1.0.0` 的版本排序更高，
可正常升级。环境变量与 CMake 基础版本不符时会拒绝构建。

## 可选 Python / paru 入口

保留便捷脚本，但它不再制作模板或源码快照。与直接 makepkg 使用同一份 PKGBUILD：

```sh
python3 scripts/package_arch.py build --syncdeps --jobs 4
python3 scripts/package_arch.py install --manager pacman --syncdeps --jobs 4
python3 scripts/package_arch.py install --manager paru --jobs 4
# 仅检查原生配方并输出 .SRCINFO 到 build/arch：
python3 scripts/package_arch.py prepare
```

脚本默认将构建缓存、软件包放入 `build/arch`，可用 `--output-dir` 指定。
不发布 AUR，不使用 `paru -S liltview` 作为当前入口。

## CI

[Release workflow](../RELEASE.md) 从完整无 Git 源码包解压后，在原生 Arch 容器中以普通
用户运行 **同一份 PKGBUILD**。它通过 `LILTVIEW_RUN_TESTS=1 makepkg --check`
显式开启完整 MinSizeRel UT，测试通过后上传标准
`.pkg.tar.zst`，并检查最终包内主程序没有调试段。
