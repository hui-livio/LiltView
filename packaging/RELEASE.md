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
| Source | all targets | `liltview-1.0.0-source.zip` and `.tar.gz` |

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

## Unified Python build entry point

`build.py` is the shared entry point for local builds and all binary CI jobs. It
requires Python **3.11+** and only the standard library. Windows/macOS/source CI
select Python 3.12 through `actions/setup-python`; Linux containers install their
native Python 3 package alongside CMake and the compilers.

From a checkout with initialized dependencies, or an extracted complete source archive:

```sh
# Configure, compile, and run Release UT (default).
python3 build.py --parallel 4

# CI keeps compilation and tests as separate steps for readable logs/reports.
python3 build.py build --parallel 4
python3 build.py test --test-parallel 2 --junit build/test-results.xml

# Application only, without compiling or running UT.
python3 build.py build --preset app-release --parallel 4

# Arch CI, as a normal user: Release build + UT + native pkg.tar.zst.
python3 build.py arch --parallel 3
```

On Windows use `python` instead of `python3` from an **x64 MSVC developer
terminal**; CI loads the same MSVC environment before invoking the script. Windows
UT run serially by default because native window tests share the desktop. On
macOS the script selects Homebrew LLVM 20 for C, and Xcode Clang for C++ and
Objective-C. Linux uses the distribution compiler.

The script uses the existing `release-ci` and `app-release` CMake presets. Extra
cache settings use repeatable `-DNAME=VALUE` arguments; quote the whole argument
when its value contains spaces. `--build-dir` overrides the default
`build/<preset>` directory for CMake builds. `VERSION_SUFFIX` or
`--version-suffix=-rc.1` selects an RC, while `--version-suffix=` selects stable;
without an override the source archive's `RELEASE_VERSION` marker applies.
CMake/CTest errors stop the command and propagate a nonzero exit code to CI.

Arch delegates to the checked-in PKGBUILD with `LILTVIEW_RUN_TESTS=1` and
`makepkg --check`; direct `makepkg -si` still defaults to no UT. Native packaging,
signing and artifact verification remain platform-specific CI steps.

## Complete source archive

GitHub's automatically generated “Source code” downloads do not contain submodule
contents and cannot currently be disabled or removed from the release page.
Use the separately uploaded `liltview-1.0.0-source.zip` (Windows) or
`liltview-1.0.0-source.tar.gz` (Linux/macOS) instead. Both contain all tracked third-party sources, including nested PlutoVG and Catch2,
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

Both formats contain the same normalized snapshot, including UTF-8 Chinese filenames.
Windows CI uses PowerShell `Expand-Archive` on the ZIP, not Windows tar or an
external unzip command. Linux/macOS use the tarball. All binary jobs build this
snapshot **without a Git checkout**, which checks
that application and third-party CMake builds work from the source distribution.
File ownership/timestamps/permissions and archive metadata are normalized for
reproducible archives. System compilers, SDKs and Linux development packages are
still required; the archive contains project dependencies, not an operating-system
sysroot.

## Self-contained resources and symbols

Windows embeds the original HarmonyOS Sans font and full redistribution notices,
uses static vendor libraries and the static MSVC CRT, and publishes only the EXE.
Embedded notices use bounded byte-escaped literals to support VS 2022/MSVC 14.44
without truncating licenses or requiring external text files.
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
