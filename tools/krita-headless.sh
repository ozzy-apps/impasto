#!/bin/sh
# Runs Krita (the flatpak org.kde.krita) without a window and with settings of
# its own in a throwaway folder next to this script's work folder, so that a
# check of Impasto's PSD and OpenRaster files never touches the Krita you
# use. Flatpak puts back its own settings folders whatever `flatpak run` is
# told, so they are set again inside the sandbox before Krita starts.
#
#   tools/krita-headless.sh WORKDIR --export --export-filename WORKDIR/out.png WORKDIR/in.psd
#
# WORKDIR must hold the files; Krita is given access to it and nothing else
# new. Its settings end up in WORKDIR/krita-home.
set -e
D=$(cd "$1" && pwd)
shift
mkdir -p "$D/krita-home/config" "$D/krita-home/data" "$D/krita-home/cache"
exec flatpak run --nosocket=x11 --nosocket=wayland --unset-env=DISPLAY --filesystem="$D" \
  --env=QT_QPA_PLATFORM=offscreen --env=QT_QPA_PLATFORMTHEME=none --env=HOME="$D/krita-home" \
  --command=sh org.kde.krita -c 'export XDG_CONFIG_HOME="$HOME/config" XDG_DATA_HOME="$HOME/data" XDG_CACHE_HOME="$HOME/cache"; exec krita --nosplash "$@"' krita "$@"
