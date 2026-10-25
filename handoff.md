# Impasto 0.12.0 — handoff

## Current status
**Complete and published:** https://github.com/ozzy-apps/impasto/releases/tag/v0.12.0

Release ID: 423606714. Source/tag commit: `2eda0e73e1630f5a2ca82044237580c3cafc39e8`. All five packages and six checksum files uploaded; all 11 server SHA-256 digests match local files. Public release and latest-release endpoints verified anonymously. The follow-up documentation commit records publication without changing release application code.

## Project vision and rules
A polished desktop painting studio for Linux and Windows, with responsive pen input, rich layers, animation and 3D reference tools. Preserve document compatibility and user artwork. Use Material 3 as the template for new UI. Keep UI text functional. Update handoff.md at each meaningful step so another AI can continue.

## Stack and architecture
- C++20, Qt 6 Widgets/Core/Gui/OpenGL/OpenGLWidgets/Svg, OpenGL 4.3, zlib.
- CMake/Ninja build; CPack DEB/ZIP/NSIS; linuxdeploy/appimagetool; Flatpak KDE 6.10.
- `src/main.cpp`: entry and developer/check commands.
- `src/engine`, `src/gfx`, `resources/shaders`: GPU painting and compositing.
- `src/brush`: brushes and stroke engine; `src/io`: documents and interchange.
- `src/ui`: interface, theme and tablet input. New shared dialog: `AboutDialog.*`.
- `src/SelfTest.cpp`: engine, interchange, UI, and new product screenshot/About checks.

## Completed steps
1. **Discovery:** inspected original clean repository, version sources, packaging, previous toolchain, and live GitHub releases. Linux v0.9.0 and Windows v0.11.1 were published separately. Existing tags/releases remain intact.
2. **About:** gallery ⋯ → About Impasto opens a rounded Material 3 dialog. Shows logo, application version, C++20/Qt 6/OpenGL, Ozzy credit with heart, support mailto and actual GitHub link. Existing canvas About uses the same dialog. Close and Escape work; artwork is preserved.
3. **Version:** CMake is canonical at 0.12.0. Package names, application version, PE resources, and now the generated Windows README derive from it. Latest AppStream release metadata agrees. Support address updated in package contact.
4. **Product README:** concise product copy and freshly captured painting/brush/gallery screenshots. Former detailed README preserved as `docs/guide.md`. Real layered landscapes were created through CanvasEngine; source screenshots and reproduction notes are under `docs/product`.
5. **Builds:** rebuilt Linux DEB, AppImage and Flatpak; Windows x64 portable ZIP and NSIS installer. Added bounded build concurrency and optional local AppImage runtime support.
6. **Verification:** full Linux engine and UI suites passed (UI 61 seconds). Windows engine and portable UI suites passed under Wine (UI 75 seconds, 85 captures, extracted into a path with spaces and é). Actual DEB/AppImage/Flatpak product/About checks passed. Flatpak bundle installed, ran, then was uninstalled. Windows installer installed, ran, and uninstalled; its executable matched the stage and an external artwork was preserved byte-for-byte. ZIP integrity, 26-file Windows DLL audit, PE version, AppStream, relative documentation links and checksums passed.
7. **Publication:** committed source and new annotated v0.12.0 tag, pushed to main atomically, staged/uploaded packages, checked all server digests, published and verified public access.
8. **Delivery:** original repository will be fast-forwarded to the final documentation commit. New packages/checksums copied into its dist folder while retaining old packages. This task also retains the complete working checkout and verification reports.

## Locations
- Actual repository: `/home/ozzy/Documents/creative-projects/sketchbook`.
- Remote: https://github.com/ozzy-apps/impasto.
- This task: `/home/ozzy/Documents/Codex/2026-10-25/referenced-chatgpt-conversation-this-is-an`.
- Isolated working clone: this task's `outputs/impasto`, branch `release/0.12.0`.
- Downloads: working clone's `dist`; original repository's `dist` after synchronization.
- Reports: this task's `outputs/release-verification.json`, `outputs/Impasto-0.12.0-validation.json`; detailed logs/results in `work/validation`.
- Historical 0.11.1 port notes: `docs/windows-port-handoff.md`. Their draft-release status is historical; 0.12.0 is published.

## Build and check instructions
Linux requires Qt 6.4+, C++20, CMake, Ninja, zlib and OpenGL 4.3:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build-release --parallel 4
build-release/impasto --selftest /tmp/impasto-selftest.png
build-release/impasto --ui-shot /tmp/impasto-ui
build-release/impasto --product-shots /tmp/Impasto-0.12.0
packaging/build-packages.sh deb appimage flatpak
```

Checks isolate settings and use their own artworks. A working desktop/OpenGL session is required. `--product-shots` creates sample layered documents and checks the gallery About entry, version/logo/stack/credit/links, Close, Escape, and unchanged artwork.

Set `CMAKE_BUILD_PARALLEL_LEVEL` to change package build concurrency (default 4). Set `IMPASTO_APPIMAGE_RUNTIME` to a verified local type-2 runtime for offline packaging. Runtime used here: SHA-256 `617aa77314d57c8b8e4fae9c3f0b35af7b30d5529cb69e98f64f92510ed98a46` from official AppImage/type2-runtime continuous release.

Native Windows: `packaging/build-windows.ps1 -QtDir <matching Qt kit> -Installer -SelfTest`; see `packaging/windows/BUILD.md`.

This release's Linux build used Qt 6.4.2; Flatpak used KDE Qt 6.10. Windows used MinGW GCC 13 POSIX, official Qt 6.4.3 and zlib 1.3.1. Cross tools are under `/tmp/impasto-windows-toolchain`; they are temporary and may disappear. Host Qt Core/Gui/Widgets Tools were selected explicitly from `/usr/lib/x86_64-linux-gnu/cmake`, with `QT_HOST_PATH=/usr`, `QT_NO_PACKAGE_VERSION_CHECK=TRUE`, `IMPASTO_DEPLOY_QT=OFF`, and local zlib source. Build/stage/config are this task's `work/windows-build`, `work/windows-stage`, `work/windows-package-config.cmake`. Prior audited Qt/compiler/WebP runtime files were reused with the newly built executable, generated README, and licenses. Wine checks used `/tmp/impasto-windows-toolchain/wineprefix`, never the user's normal profile.

## Roadmap and remaining limits
No required release work remains once local synchronization is confirmed. Next optional work, in order:
1. Native Windows hardware validation: tablet pressure/tilt/eraser, GPU drivers, high-DPI and multiple displays. Wine checks do not certify these paths.
2. Windows signing and automated multi-platform CI. Current packages are unsigned.
3. Continue painting features without breaking `.imp`, legacy `.sbk`, PSD/PSB or ORA compatibility. Preserve the concise README; put detailed tool documentation in the guide.

For future releases, choose one new CMake version, add AppStream history, rebuild every platform, verify packages and server digests, and create a new tag. Do not reuse/move the historical v0.11.1 tag, which points to the pre-Windows-port commit.
