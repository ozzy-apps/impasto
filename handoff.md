# Impasto 0.12.0 — release handoff

## Vision and constraints
A polished desktop painting studio for Linux and Windows. Preserve the painting workflow, document compatibility, and user artwork. UI copy must be functional; new UI follows Material 3. This release aligns all packages at 0.12.0, adds About in the gallery, and refreshes the product README with genuine app screenshots.

## Stack and architecture
C++20, Qt 6 Widgets/Core/Gui/OpenGL/OpenGLWidgets/Svg, OpenGL 4.3, zlib, CMake/Ninja/CPack. Entry: src/main.cpp; GPU painting: src/engine, src/gfx, resources/shaders; brush engine: src/brush; document interchange: src/io; interface: src/ui. Built-in engine/UI checks live in src/SelfTest.cpp.

## Step 1 — discovery completed
- Original repository: /home/ozzy/Documents/creative-projects/sketchbook, clean main at 0737902.
- Remote: https://github.com/ozzy-apps/impasto. GitHub credentials have push/admin access.
- Fresh isolated checkout in this task at outputs/impasto, branch release/0.12.0. Original checkout remains untouched during implementation.
- Live GitHub inspection: Linux v0.9.0 release contains DEB/AppImage/Flatpak; Windows v0.11.1 release contains ZIP/NSIS installer. Both published. Existing v0.11.1 tag points to the pre-port commit, so do not reuse or move it.
- Chosen new version: 0.12.0. All rebuilt packages and a new tag will reference the same tested source commit.

## Step 2 — implementation completed; verification in progress
- Added shared src/ui/AboutDialog.* with logo, application version, actual C++20/Qt 6/OpenGL stack, Ozzy credit, support mailto, and verified GitHub URL. Accessible from gallery overflow and existing canvas About entry.
- Rounded tonal dialog, 24 px insets, 48-ish pixel text-button target, keyboard dismissal, selectable links.
- CMake version is 0.12.0. Windows README is now generated from CMake using README.txt.in; PE version and package names already derive from CMake. Added AppStream release metadata.
- Preserved the former detailed README as docs/guide.md and replaced README.md with concise product copy and fresh screenshot references.
- Added --product-shots <directory>: isolated settings, sample layered paintings created by the real GPU engine, screenshots of real widgets/framebuffer, end-to-end gallery About/version/logo/link/Close verification.
- Fresh Linux and Windows cross-builds are running. No build or publication is claimed complete yet.

## Build environment
Linux Qt 6.4.2 and host tools installed. Windows official Qt 6.4.3, MinGW GCC 13 POSIX and NSIS extracted in /tmp/impasto-windows-toolchain. Matching prior runtime stage available in original dist/windows-stage with Qt, compiler, WebP runtime and licenses. Rebuild executable and generated README, retain audited runtime files. Native Windows hardware/pressure/tilt/high-DPI verification remains future work; Wine is available for compatibility testing.

## Step 3 — builds and screenshots
- Linux Release compiled successfully. Initial product/About verification passed and four layered sample artworks were created. Visually inspected painting, gallery and About captures.
- Windows cross-build configured with host Qt tool overrides plus QT_NO_PACKAGE_VERSION_CHECK=TRUE (Linux host 6.4.2 tools, Windows Qt 6.4.3); initial executable compiled. Final incremental compile includes dialog refinements and Escape check.
- Full Linux engine/interchange checks are running; final screenshot checks, Linux DEB/AppImage packaging, and Flatpak builder inspection have started.

## Step 4 — verification and packaging in progress
- Full Linux engine test exited 0. Final Linux product/About check exited 0, including logo, version, stack, credit, both URLs, Close, Escape, and unchanged artwork. Fresh README screenshots copied to docs/product and provenance recorded there.
- Windows final Release executable compiled and installed into a stage containing the prior verified Qt/compiler/WebP runtime. Product/About checks exited 0 under Wine and produced fresh screenshots showing 0.12.0.
- DEB and Windows ZIP built. NSIS installer, AppImage and Flatpak are building. AppImage initially could not fetch its runtime inside restricted networking; downloaded official runtime with verified GitHub digest and added IMPASTO_APPIMAGE_RUNTIME support for offline packaging.
- Recursive audit of 26 Windows PE files found no missing non-system dependencies; PE version resource contains 0.12.0. Evidence: work/validation/dll-audit.json.
- Native Windows hardware validation remains unperformed.

## Step 5 — packages built and deployment checks
- Built all five 0.12.0 packages: Linux DEB, AppImage, Flatpak, Windows portable ZIP and NSIS installer. Windows ZIP integrity passed, and its executable/README match the fresh stage. DEB executable matches the tested Linux build byte-for-byte.
- DEB and AppImage deployed product/About runs both returned 0. Flatpak staged app run on KDE Platform 6.10 returned 0; bundle import verification is in progress.
- Full Linux interface suite repeated with a result receipt: exit 0 in 61 seconds, including painting/panels, gallery/recovery, layered files, animation and 3D references. Windows full engine test under Wine exited 0; full portable UI and installer checks are running.
- Final screenshot provenance and local documentation links validated.

## Step 6 — final validation
- Full Windows portable UI suite: exit 0 in 75 seconds under Wine, from an extracted path containing spaces and é; 85 screenshots produced.
- NSIS silent install, installed application/About run, and uninstall all returned 0. Installed executable matched the stage; external sample artwork was preserved byte-for-byte.
- Flatpak bundle created and imported. Runtime/About check passed on KDE 6.10; actual bundle install/run/uninstall verification all returned 0.
- AppStream metadata validation passed. Canonical CMake version matches latest AppStream release. All local README/guide links resolve. SHA-256 manifest and sidecars generated for all five downloads.

## Step 7 — publication in progress
All requested implementation and local verification are complete. Preparing one source commit and v0.12.0 tag, then a draft with the five verified packages and checksums. No new public release is claimed until GitHub publication and digest verification succeed.

## Release next steps
1. All validation is complete; commit source and tag v0.12.0.
2. Push source/tag, upload all five packages plus checksums to a new draft, compare server digests, publish, verify public release.
3. Record exact release commit/URL and fast-forward the original checkout safely; copy only new 0.12.0 packages to its dist folder.

## Roadmap after release
1. Test native Windows GPU drivers, tablet pressure/tilt/eraser, and multiple/high-DPI displays on physical hardware.
2. Consider signing Windows packages and adding reproducible multi-platform CI.
3. Keep package version, About, PE resources and generated README derived from CMake; add each new release to AppStream metadata.

## Reproduction instructions for the next AI
- Linux configure/build: `cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr`; `cmake --build build-release --parallel 4`.
- Engine/UI checks: `build-release/impasto --selftest <output.png>`; `build-release/impasto --ui-shot <prefix>`; new product/About checks: `build-release/impasto --product-shots <directory>`. All use isolated settings. A desktop session with OpenGL 4.3 is required.
- Linux packages: `packaging/build-packages.sh deb appimage flatpak`. Set `IMPASTO_APPIMAGE_RUNTIME` to a verified local runtime to avoid a runtime download; `CMAKE_BUILD_PARALLEL_LEVEL` defaults to 4.
- Windows native build: `packaging/build-windows.ps1 -QtDir <Qt kit> -Installer -SelfTest`. See packaging/windows/BUILD.md.
- Actual cross-build here: toolchain `/tmp/impasto-windows-toolchain/toolchain.cmake`, Windows prefix `/tmp/impasto-windows-toolchain/qt/6.4.3/mingw_64`, `QT_HOST_PATH=/usr`, `QT_HOST_PATH_CMAKE_DIR=/usr/lib/x86_64-linux-gnu/cmake`, host `Qt6CoreTools_DIR`, `Qt6GuiTools_DIR`, `Qt6WidgetsTools_DIR` under that same cmake directory, `QT_NO_PACKAGE_VERSION_CHECK=TRUE`, `IMPASTO_DEPLOY_QT=OFF`, and local `FETCHCONTENT_SOURCE_DIR_IMPASTO_ZLIB=/tmp/impasto-windows-toolchain/zlib-1.3.1`.
- Cross-build directory is this chat's work/windows-build; package stage work/windows-stage. Runtime files reused from the prior audited stage, with freshly installed executable, README, and license files. CPack wrapper work/windows-package-config.cmake uses the extracted NSIS and host-compatible template under /tmp/impasto-windows-toolchain/cpack.
- Wine tests use the disposable prefix `/tmp/impasto-windows-toolchain/wineprefix`, never the user's normal Wine profile. Evidence and receipts are under this chat's work/validation. Screenshots and reproduction notes are committed in docs/product.

## Prior port history
The previous Windows implementation and validation notes are preserved below as historical evidence. They describe 0.11.1, not proof of 0.12.0 verification.

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
