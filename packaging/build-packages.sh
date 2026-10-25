#!/usr/bin/env bash
# Builds the three packages into dist/: a .deb (for Ubuntu 24.04 and what is
# built on it), an AppImage and a Flatpak bundle.
#
# Needs, besides what building needs (see the README):
#   .deb      dpkg-dev (for dpkg-shlibdeps)
#   AppImage  linuxdeploy, linuxdeploy-plugin-qt and appimagetool, as AppImages in
#             ~/.cache/impasto-packaging (from github.com/linuxdeploy and github.com/AppImage)
#   Flatpak   flatpak install --user flathub org.flatpak.Builder org.kde.Sdk//6.10
#
# Set IMPASTO_APPIMAGE_RUNTIME to a verified local type-2 runtime for offline packaging.
# CMAKE_BUILD_PARALLEL_LEVEL controls build jobs (default 4).
#
# Usage: packaging/build-packages.sh [deb] [appimage] [flatpak]   (all three when none is named)
set -euo pipefail
cd "$(dirname "$0")/.."
root=$PWD
version=$(sed -n 's/^project(Impasto VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
tools=${IMPASTO_PACKAGING_TOOLS:-$HOME/.cache/impasto-packaging}
wanted=${*:-deb appimage flatpak}
mkdir -p dist

if [[ $wanted == *deb* || $wanted == *appimage* ]]; then
    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
    cmake --build build-release --parallel "${CMAKE_BUILD_PARALLEL_LEVEL:-4}"
fi

if [[ $wanted == *deb* ]]; then
    (cd build-release && cpack -G DEB)
    mv build-release/impasto_"$version"_*.deb dist/
fi

if [[ $wanted == *appimage* ]]; then
    rm -rf AppDir
    DESTDIR=$root/AppDir cmake --install build-release
    export APPIMAGE_EXTRACT_AND_RUN=1          # works without FUSE
    export QMAKE=${QMAKE:-$(command -v qmake6 || command -v qmake)}
    export LINUXDEPLOY_OUTPUT_VERSION=$version
    # Wayland as well as X11, where the Qt Wayland plugins are installed.
    if [[ -d /usr/lib/x86_64-linux-gnu/qt6/plugins/wayland-shell-integration ]]; then
        export EXTRA_PLATFORM_PLUGINS="libqwayland-egl.so;libqwayland-generic.so"
        export EXTRA_QT_MODULES="waylandcompositor"
    fi
    export PATH=$tools:$PATH
    "$tools/linuxdeploy-x86_64.AppImage" --appdir AppDir \
        --desktop-file AppDir/usr/share/applications/io.github.ozzy_apps.Impasto.desktop \
        --icon-file AppDir/usr/share/icons/hicolor/256x256/apps/io.github.ozzy_apps.Impasto.png \
        --plugin qt
    # appimagetool rather than linuxdeploy's own output step, so the name is ours.
    runtime_args=()
    if [[ -n ${IMPASTO_APPIMAGE_RUNTIME:-} ]]; then
        runtime_args=(--runtime-file "$IMPASTO_APPIMAGE_RUNTIME")
    fi
    ARCH=x86_64 "$tools/appimagetool-x86_64.AppImage" --no-appstream "${runtime_args[@]}" AppDir "dist/Impasto-$version-x86_64.AppImage"
fi

if [[ $wanted == *flatpak* ]]; then
    flatpak run org.flatpak.Builder --user --jobs="${CMAKE_BUILD_PARALLEL_LEVEL:-4}" --force-clean --repo=build-flatpak/repo --state-dir=build-flatpak/state \
        build-flatpak/app packaging/flatpak/io.github.ozzy_apps.Impasto.yml
    flatpak build-bundle build-flatpak/repo "dist/Impasto-$version.flatpak" io.github.ozzy_apps.Impasto \
        --runtime-repo=https://dl.flathub.org/repo/flathub.flatpakrepo
fi

ls -la dist
