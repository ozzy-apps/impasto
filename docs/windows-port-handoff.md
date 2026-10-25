# Historical Windows 0.11.1 port handoff

These notes predate the unified 0.12.0 release. See the root handoff.md for current status.

# Impasto Windows port

## Project
Impasto 0.11.1 is a desktop painting application built with C++20, Qt 6 Widgets, OpenGL 4.3, zlib, and CMake. Its existing source repository is the parent directory, `../`; this workspace is its `dist/` directory containing Linux releases. Preserve the existing painting workflow and document compatibility. Any new UI should follow Material 3 and contain functional text only.

## Step 1 — discovery
- Inspected the existing source, build configuration, packaging, and built-in self-tests.
- Existing distribution targets are Linux DEB, Flatpak, and AppImage.
- Identified unconditional GCC warning flags, Linux installation rules, and Windows packaging gaps.
- Found Wine, MinGW C tools, Qt 6.4 host tools, and Linux Qt development packages; the MinGW C++ compiler and Windows Qt libraries must be obtained for a real Windows build.
- Downloaded the official Qt 6.4.3 package index to `/tmp/impasto-qt-updates.xml`.

## Step 2 — implementation in progress
- Staged a complete portable source tree in `windows-source/` because the parent source repository is outside the workspace write boundary.
- Added Windows-aware CMake compiler options, an application icon and version resource, desktop OpenGL selection, Windows ZIP/NSIS packaging, and Qt deployment through `windeployqt`.
- Removed unconditional Linux backtrace headers and replaced nonstandard `M_PI` with C++20 `std::numbers::pi`. OpenGL extension function pointers use the platform calling convention.
- Added `packaging/build-windows.ps1` for native Windows builds and a functional Windows README. Windows builds can fetch a pinned zlib source when no zlib development package is installed.
- Obtained official Windows Qt 6.4.3 libraries and Ubuntu MinGW GCC 13/NSIS packages, extracted only under `/tmp/impasto-windows-toolchain/`; no system packages were installed.
- A real Windows cross-build is running in `windows-build/`; log: `/tmp/impasto-windows-build.log`. Host Qt 6.4.2 tools are selected explicitly for code generation against Windows Qt 6.4.3.

## Step 3 — binaries and validation
- Windows x64 `windows-stage/impasto.exe` compiled successfully with MinGW GCC 13, Windows Qt 6.4.3, and static zlib 1.3.1.
- Linux `linux-validation/impasto` compiled successfully and its full `--selftest` returned exit code 0. Log: `/tmp/impasto-linux-selftest.log`.
- The deployed Windows binary's full `--selftest` returned exit code 0 under Wine 10.0. Output documents/images are in `windows-build/windows-selftest.png*`. Windows GUI logging did not appear in redirected output for this run; the exit code and generated outputs were verified.
- Windows `--ui-shot` checks are running and generating screenshots in `windows-build/ui-*.png`; visually inspected `ui-main.png`, which rendered correctly.
- Added a Windows manifest for per-monitor DPI, Windows 10/11 compatibility, and normal user execution. The version resource is generated from the CMake project version.
- Bundled Qt and compiler runtime DLLs, licenses, and `qt.conf` with relative paths. Fixed Qt 6.4's unreliable MinGW plugin debug/release detection by using automatic detection for MinGW. Avoid distributing Wine's D3D compiler stub; Windows 10/11 supply system graphics libraries.
- DLL import auditing found missing external WebP libraries in the official Qt image-format plugin. An optional standalone `packaging/windows/webp-runtime/` CMake helper builds compatible libwebp 1.6.0 DLLs; that build is running in `windows-webp-build/`.
- Exact matching Qt 6.4.3 source archive licenses/attributions are included under `packaging/windows/licenses/Qt/`.

## Step 4 — completed package verification
- Created `windows-packages/Impasto-0.11.1-windows-x64.zip` and the NSIS `.exe` installer. The ZIP was integrity-tested and extracted outside the source/build directory, under a path containing spaces and an accented character.
- Windows full UI suite now returns exit code 0 under Wine. It exercises pen/mouse interactions, shapes, panels, palettes, dialogs, gallery/recovery, layered files, animation, and the 3D reference panel. The Windows platform hid dialog button icons by default; the shared Fusion proxy style now explicitly enables them. Visually inspected the main canvas screenshot.
- A separate runtime probe using the bundled Qt DLLs passed PNG, JPEG, WebP and TIFF round trips plus SVG loading and SVG icon rendering. The same probe passed against the extracted ZIP.
- All 26 shipped PE executables/libraries passed recursive import auditing with no missing non-system DLLs. Audit report: `windows-build/dll-audit.json`.
- The installer completed successfully in a temporary Wine prefix and created Start Menu/uninstaller shortcuts. The installed application successfully opened and rendered a PSD. The extracted portable application also opened a PSD with spaces/accented characters in its path and rendered it correctly.
- Linux full engine self-test passes. The Linux 3D UI test fails its existing "Keep" assertion in both the untouched `../build-release/impasto` baseline and the port build (each exit 2); this is not introduced by this port. Windows full UI passes that assertion. Logs: `/tmp/impasto-linux-model-baseline.log` and `/tmp/impasto-linux-model-port.log`.
- The host NSIS compiler cannot use CPack's Windows-only BrandingText trimming option, so the cross-package configuration uses a temporary template with an equivalent blank branding line. Native Windows CPack builds use the normal template.
- Uninstall returned exit code 0, removed the application and shortcuts, and preserved a sample artwork in an external private gallery byte-for-byte. The private gallery was entirely inside the temporary Wine prefix.

## Remaining release follow-up
Native Windows hardware validation remains for pressure, tilt, eraser, high-DPI/multiple monitors, and GPU drivers. This Linux environment can test Windows binaries under Wine, but cannot certify those hardware paths. Code signing and automated Windows CI can be added for future public releases.

## Working paths and project architecture
- Source: parent project directory `/home/ozzy/Documents/creative-projects/sketchbook`.
- This task's writable staging/output directory: `/home/ozzy/Documents/creative-projects/sketchbook/dist`.
- Port source snapshot: `dist/windows-source/`; cross-build: `dist/windows-build/`; runtime stage: `dist/windows-stage/`; Linux validation: `dist/linux-validation/`.
- Main entry: `src/main.cpp`; GPU engine: `src/engine/CanvasEngine.*`, `src/gfx/Gl.*`; painting/brushes: `src/brush/`; interchange: `src/io/`; UI/theme/tablet input: `src/ui/`; shaders and built-in assets: `resources/`.
- Existing engine tests and UI interaction tests live in `src/SelfTest.cpp` and run through `--selftest <png>` and `--ui-shot <prefix>`.
- Native Windows build: `packaging/build-windows.ps1`; detailed instructions: `packaging/windows/BUILD.md`; standalone external WebP helper: `packaging/windows/webp-runtime/`.
- Downloaded/extracted build tools, external sources, isolated Wine profile, and cross-package template: `/tmp/impasto-windows-toolchain/`. No host packages were installed.

## Finalization
The implementation, Windows package builds, and compatibility checks are complete. The reviewed source snapshot has been synchronized into the parent project. Delivery artifacts are `dist/Impasto-0.11.1-windows-x64.exe`, `dist/Impasto-0.11.1-windows-x64.zip`, and `dist/Impasto-0.11.1-windows-source.zip`. SHA-256 checksums are stored beside the downloads and in `dist/SHA256SUMS-Windows.txt`. Preserve existing Linux release files.

For the next AI: no further implementation is required for this Windows build. Hardware validation and signing are future release work. Do not report native Windows or physical tablet testing unless it actually happened.

## Step 5 — delivery
- Source changes are synchronized into the main project, including `handoff.md` and native Windows build instructions.
- Existing Linux distribution files are preserved. No Git commit, push, release publication, or staging was performed.
- Windows installer and portable ZIP are copied into the main `dist/` directory. The source archive is created from the exact tested source snapshot; SHA-256 sidecars verify all three downloads.
- Windows full engine and UI suites returned 0. Portable image-plugin checks, Unicode/spaces PSD import, install/installed-app/uninstall, and artwork preservation passed. The unchanged baseline reproduces the separate Linux 3D "Keep" UI assertion noted above.
- All requested local build work is complete. Future hardware validation is listed under Remaining release follow-up.

## Step 6 — repository publication complete
- The user requested pushing the Windows port to `https://github.com/ozzy-apps/impasto`.
- Fetched `origin/main`, which matched local `main` at `227050c`; no merge or rebase was needed.
- Committed the Windows port as `ced2b09f6dba7cd08cd70342b6a81c77f80d65c6` (`Add Windows x64 builds and installer packaging`) and pushed it to `origin/main`.
- Application/build files pass the staged whitespace check. Third-party license notices retain their upstream formatting. The engine/UI/package validation from Step 5 remains applicable; no application code changed during publication.
- Created the draft `v0.11.1` release (GitHub release ID `419162909`), targeting the exact port commit. The existing published `v0.9.0` release remains intact.
- Draft release: https://github.com/ozzy-apps/impasto/releases/tag/untagged-b589f506391227dfef54
- Uploaded the Windows installer, portable ZIP, source archive, three individual SHA-256 files, and `SHA256SUMS-Windows.txt`. All seven uploaded assets have server-reported SHA-256 digests matching the local files.
- Verification receipt: `dist/windows-verification/github-draft-release.json`. Build artifacts remain excluded from Git under `dist/`.
- The draft remains unpublished. Future release work consists of native Windows hardware validation, optional signing, and publishing the draft when requested. The source archive preserves the tested build snapshot; its embedded handoff predates this publication step.
