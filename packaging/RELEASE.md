# LiltView 1.0.0 release

The GitHub Actions **Release** workflow builds native packages on Windows, macOS,
Ubuntu, Fedora and Arch. All configurations use **Release**, including the full
Catch2 UT and C ABI/ownership contracts. Test failures block artifact creation.
Tests and debug files are never installed or published.

| Platform | Architecture / minimum target | Release asset |
| --- | --- | --- |
| Windows | x64, Windows 10+ | `LiltView-1.0.0-windows-x64.exe` |
| macOS | Apple Silicon, macOS 15+ | `LiltView-1.0.0-macos-arm64.dmg` and `.zip` |
| macOS | Intel, macOS 15+ | `LiltView-1.0.0-macos-x86_64.dmg` and `.zip` |
| Ubuntu | 26.04, x86_64 | `liltview-1.0.0-linux-ubuntu26.04-x86_64.deb` |
| Fedora | 44, x86_64 | `liltview-1.0.0-linux-fedora44-x86_64.rpm` |
| Arch Linux | rolling, x86_64 | `liltview-1.0.0-1-x86_64.pkg.tar.zst` |
| Source | all targets | `liltview-1.0.0-source.tar.gz` |

Ubuntu/Fedora packages target the named distribution and ABI, not every older
Debian/RPM system. macOS architectures are built independently, not Universal 2.
Windows uses the Win32 platform API but the artifact is **64-bit**, not x86/32-bit.

## Validate before release

Push the branch and open a PR, or select **Actions → Release → Run workflow**.
Download the `release-*` workflow artifacts and the `test-results-*` reports.
Manual/PR runs build and test everything without creating a GitHub Release.
A successful workflow, plus manual launch/open-image checks on each desktop,
is required before claiming a platform has been validated.

For a local Release build with UT:

```sh
cmake --preset release-ci
cmake --build --preset release-ci --parallel 4
ctest --preset release-ci
```

On Windows run this from an x64 MSVC Developer Shell and add
`-DCMAKE_C_COMPILER=cl.exe -DCMAKE_CXX_COMPILER=cl.exe` to configure.
CI passes the platform-specific compilers and dependency settings explicitly.
The release packaging option rejects non-Release configurations.

## Pre-release first, then stable

The base CMake version stays **1.0.0**. Manual/PR workflow runs default to
**1.0.0-rc.1**; versioned tags select either an RC or the stable version. CLI and
Windows string version metadata carry the suffix, while numeric EXE/macOS
bundle version fields remain 1.0.0.

First, after CI validation, create a release-candidate tag:

```sh
git tag -a v1.0.0-rc.1 -m "LiltView 1.0.0 release candidate 1"
git push origin v1.0.0-rc.1
```

The workflow creates a **draft Pre-release**, not a stable/latest release. Review
and publish that preview, then collect desktop feedback. Use `v1.0.0-rc.2`, etc.
for additional candidates. RC filenames include `1.0.0-rc.1`; native versions use
`1.0.0~rc.1` for DEB/RPM and `1.0.0rc.1` for Arch, so the stable package upgrades
correctly over an RC. The source archive keeps the same RC prefix as its filename.

## Publish stable only after RC acceptance

After the workflow and desktop checks pass on the intended commit:

```sh
git tag -a v1.0.0 -m "LiltView 1.0.0"
git push origin v1.0.0
```

Stable tags must equal the CMake project version; RC tags must be `vVERSION-rc.N`. The workflow builds/tests every
platform, computes `SHA256SUMS`, and creates a **draft** GitHub Release containing
all native assets and the complete source archive. Review the draft release notes
and artifacts, then publish it in GitHub. No tag or release is created by local
packaging scripts.

## Complete source archive

GitHub's automatically generated “Source code” downloads do not contain submodule
contents. Use the separately uploaded `liltview-1.0.0-source.tar.gz` instead. It
contains all tracked third-party sources, including nested PlutoVG and Catch2,
and their licenses. A plain `RELEASE_VERSION` marker preserves RC identity for
offline builds (no Git history is stored). It excludes `.git`, `.gitmodules`, other `.git*` administrative
files, `.github`, untracked files and build outputs. Dependency provenance
`source.json` and license attribution remain intact.

The archive is created from a clean checkout with every recursive submodule
initialized at its pinned commit:

```sh
git submodule update --init --recursive
python3 scripts/package_release.py source --output build/dist
```

All binary jobs build this exact archive **without a Git checkout**, which checks
that application and third-party CMake builds work from the source distribution.
File ownership/timestamps/permissions and gzip headers are normalized for
reproducible archives. System compilers, SDKs and Linux development packages are
still required; the archive contains project dependencies, not an operating-system
sysroot.

## Self-contained resources and symbols

Windows embeds the original HarmonyOS Sans font and full redistribution notices,
uses static vendor libraries and the static MSVC CRT, and publishes only the EXE.
`liltview.exe --licenses` prints its embedded licenses (redirect output to a text
file if launching from a GUI terminal). The CI tests the copied EXE from a directory
with no adjacent font files and rejects DLL CRT / SDL dependencies.

macOS installs a proper `LiltView.app`, font, icon, version metadata and licenses,
then strips and ad-hoc signs it. CI rejects Homebrew/local dylib dependencies and
creates a DMG with an Applications shortcut plus a ZIP of the app. These are
**not Developer ID signed or notarized**: no Apple signing identity has been
configured. Gatekeeper may require user approval. Homebrew LLVM 20 compiles the C23 core;
Xcode Clang builds C++/Objective-C and links system frameworks. A private pthread
adapter supplies the C11 threading subset unavailable in the Darwin SDK.

Linux packages use their distribution-native toolchains and runtime dependency
metadata (including SDL's dynamically loaded window backends). CPack strips
DEB/RPM binaries; makepkg strips the Arch package and disables a separate debug
subpackage. Neither PDB, dSYM, DWARF debug sections nor UT binaries are published.
The checked-in `packaging/arch/PKGBUILD` works directly with `makepkg -si` from
a full checkout or source archive; it is not generated from a template. Normal
Arch installs do not build or run UT (`BUILD_TESTING=OFF` and
`LILTVIEW_BUILD_UNIT_TESTS=OFF`); only version/desktop validation remains. Release CI
explicitly uses `LILTVIEW_RUN_TESTS=1 makepkg --check`: `check()` runs the full
Release UT before `package()`, and test failures block packaging.

Install packages using the matching platform package manager:

```sh
sudo apt install ./liltview-1.0.0-linux-ubuntu26.04-x86_64.deb
sudo dnf install ./liltview-1.0.0-linux-fedora44-x86_64.rpm
sudo pacman -U ./liltview-1.0.0-1-x86_64.pkg.tar.zst
```
