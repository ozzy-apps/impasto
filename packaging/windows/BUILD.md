# Windows builds

Impasto uses C++20, Qt 6.4 or newer (Core, Gui, Widgets, OpenGL,
OpenGLWidgets, Svg), CMake 3.21 or newer, Ninja, and zlib. Build a 64-bit
Release configuration. Windows 10/11 with an OpenGL 4.3 graphics driver is
required to run it.

## Native Windows

1. Install Qt's 64-bit desktop kit from the Qt installer, including Qt SVG
   and Qt Image Formats. Use either its matching MinGW compiler or an MSVC
   kit with Visual Studio 2022 C++ Build Tools. Keep Qt and compiler
   architectures/toolchains matched.
2. Install CMake and Ninja. For an installer, install NSIS 3.03 or newer.
3. Open the matching Qt/MinGW command prompt or an x64 Visual Studio developer
   PowerShell. Put Qt's `bin` directory and the compiler on `PATH`.
4. From the repository root, run:

   ```powershell
   powershell -ExecutionPolicy Bypass -File packaging/build-windows.ps1 -QtDir C:\Qt\6.4.3\mingw_64 -Installer
   ```

   Change `-QtDir` to the selected Qt kit. Omit `-Installer` to build only
   the portable ZIP. `-BuildDir` and `-OutputDir` override output directories.
   A system zlib development package is used when available; otherwise CMake
   downloads the pinned zlib 1.3.1 source from its official GitHub repository
   and links it statically. This requires network access on the first build.
   Use `-DIMPASTO_FETCH_ZLIB=OFF` when configuring manually to require an
   installed zlib, or set `FETCHCONTENT_SOURCE_DIR_IMPASTO_ZLIB` to a local
   zlib source directory for an offline build.

The script builds `impasto.exe`, and CPack stages it with `windeployqt` and
produces `Impasto-<version>-windows-x64.zip`, optionally an `.exe` installer,
and SHA-256 sidecars in `dist`. Qt, image plugins, platform plugins and the
compiler runtime are bundled. Qt's software OpenGL fallback is excluded
because it cannot provide the OpenGL 4.3 required by the painting engine.

The installer creates Start Menu and desktop shortcuts and an uninstaller.
Documents remain in the user's chosen gallery folder. This build does not
change Windows' default file associations. Packages are unsigned.

## Verification

On a Windows machine with a suitable graphics driver:

```powershell
packaging/build-windows.ps1 -QtDir C:\Qt\6.4.3\mingw_64 -Installer -SelfTest
```

After extracting the portable ZIP into a folder outside the development tree,
run the deployed binary's tests to check the actual bundled libraries:

```powershell
$p = Start-Process .\impasto.exe -ArgumentList '--selftest', 'selftest.png' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "Self-test failed: $($p.ExitCode)" }
$p = Start-Process .\impasto.exe -ArgumentList '--ui-shot', 'ui' -Wait -PassThru
if ($p.ExitCode -ne 0) { throw "UI tests failed: $($p.ExitCode)" }
```

Also check pressure, tilt, the eraser, high-DPI displays, opening paths with
spaces/non-ASCII characters, saving/reopening `.imp`, PSD/ORA import/export,
and install/uninstall on Windows hardware. The self-test uses isolated
settings, and on Windows redirects QSettings to temporary INI files rather
than the user's registry.

## Linux cross-build

A MinGW-w64 POSIX C/C++ toolchain can link against the official Windows Qt
MinGW libraries. Supply a CMake toolchain with `CMAKE_SYSTEM_NAME=Windows`,
the matching compilers and resource compiler, Qt's Windows prefix, and
Linux-host Qt tools via `QT_HOST_PATH` (use matching Qt versions). Set
`IMPASTO_DEPLOY_QT=OFF`: Windows `windeployqt.exe` needs Windows or Wine.

Build and install to a staging folder, run `windeployqt.exe
--no-compiler-runtime --no-translations --no-opengl-sw --no-system-d3d-compiler --dir <stage>
<stage>/impasto.exe` under Wine, and provide the selected compiler's runtime
DLLs. Audit imports recursively: every non-system DLL and plugin dependency
must be included. Package the complete staging directory using CPack ZIP/NSIS
with `CPACK_INSTALL_CMAKE_PROJECTS` cleared and `CPACK_INSTALLED_DIRECTORIES`
pointing to that stage. Use `CPACK_NSIS_EXECUTABLE` for a host `makensis`.

Tests under Wine are useful compatibility checks; they do not replace native
Windows graphics and physical tablet testing. See `handoff.md` for exact
build provenance and completed checks for the supplied release.

## External WebP runtime in older Qt kits

Some Qt 6.4.3 MinGW kits ship `qwebp.dll` without its external
`libwebp-7__.dll`, `libwebpdemux-2__.dll` and `libwebpmux-3__.dll` dependencies.
The optional `webp-runtime` helper builds compatible libwebp 1.6.0 libraries:

```powershell
cmake -S packaging/windows/webp-runtime -B build-webp -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-webp --parallel
packaging/build-windows.ps1 -QtDir C:\Qt\6.4.3\mingw_64 -WebPRuntimeDir build-webp\_deps\libwebp-build -Installer
```

Use the same MinGW compiler for both builds. Supply a toolchain file to the
helper for a Linux cross-build. Include `libsharpyuv.dll` as well as the three
WebP DLLs. The helper pins and verifies the official source archive. The
supplied Windows release includes these libraries and their notices.

For MinGW, let `windeployqt` automatically detect release/debug plugins;
forcing `--release` can incorrectly exclude official Qt 6.4 plugins. For MSVC,
the native install script explicitly selects Release. Do not distribute
Wine's built-in system DLLs with the Windows application.
