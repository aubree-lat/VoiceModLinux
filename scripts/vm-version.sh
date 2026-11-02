#!/bin/bash
# vm-version.sh <prefix> <voicemod-version>      e.g.  vm-version.sh prefix 3.17.20
#
# Replaces the Voicemod app in a prefix with a specific official release, taken
# from Voicemod's own update server (the same file its updater downloads).
# The previous app directory is kept as app/last.prev so nothing is lost.
set -eu
P=${1:?usage: vm-version.sh <prefix> <version>}
V=${2:?usage: vm-version.sh <prefix> <version>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CACHE=$HERE/cache/voicemod/$V
ZIP=$CACHE/Voicemod_$V-Windows_Update.zip
APP=$(ls -d "$P"/drive_c/users/*/AppData/Local/VoicemodV3/app 2>/dev/null | head -1)
[ -n "$APP" ] || { echo "vm-version: no Voicemod install in $P" >&2; exit 1; }

mkdir -p "$CACHE"
if [ ! -f "$ZIP" ]; then
    curl -fL -s -m 900 -o "$ZIP.part" "https://flagship-versions.voicemod.net/$V/Voicemod_$V-Windows_Update.zip"
    mv "$ZIP.part" "$ZIP"
fi
sha256sum "$ZIP"
unzip -Z1 "$ZIP" | grep -qx 'Voicemod.exe' || { echo "vm-version: Voicemod.exe not at the top of the zip" >&2; exit 1; }

export WINEPREFIX=$P WINEARCH=win64 WINEDEBUG=-all
wineserver -k 2>/dev/null || true

new=$(mktemp -d "$APP/last.new.XXXXXX")
unzip -q "$ZIP" -d "$new"
rm -rf "$APP/last.prev"
[ -d "$APP/last" ] && mv "$APP/last" "$APP/last.prev"
mv "$new" "$APP/last"
echo "Voicemod $V is now in $APP/last (previous build kept as last.prev)"
