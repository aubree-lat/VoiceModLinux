#!/bin/bash
# dxvk-swap.sh <prefix> <tag>     e.g.  dxvk-swap.sh prefix v2.7.1
#
# Installs a specific DXVK release into a Wine prefix (d3d11, dxgi, d3d10core;
# 64- and 32-bit). Downloads are cached under cache/dxvk and their SHA-256 is
# printed so a known-good release can be pinned.
set -eu
P=${1:?usage: dxvk-swap.sh <prefix> <tag>}
TAG=${2:?usage: dxvk-swap.sh <prefix> <tag>}
HERE=$(cd "$(dirname "$0")/.." && pwd)
CACHE=$HERE/cache/dxvk
mkdir -p "$CACHE"
ver=${TAG#v}
tgz=$CACHE/dxvk-$ver.tar.gz

if [ ! -f "$tgz" ]; then
    url=$(curl -s -m 20 "https://api.github.com/repos/doitsujin/dxvk/releases/tags/$TAG" |
          python3 -c 'import json,sys; print(next(a["browser_download_url"] for a in json.load(sys.stdin)["assets"] if a["name"].endswith(".tar.gz") and "native" not in a["name"]))') ||
        { echo "dxvk-swap: cannot find release $TAG" >&2; exit 1; }
    echo "downloading $url"
    curl -sL -m 240 -o "$tgz.part" "$url" && mv "$tgz.part" "$tgz"
fi
sha256sum "$tgz"

export WINEPREFIX=$P WINEARCH=win64 WINEDEBUG=-all
wineserver -k 2>/dev/null || true

tmp=$(mktemp -d)
tar xzf "$tgz" -C "$tmp"
d=$(ls -d "$tmp"/dxvk-*)
for dll in d3d11 dxgi d3d10core; do
    cp "$d/x64/$dll.dll" "$P/drive_c/windows/system32/"
    cp "$d/x32/$dll.dll" "$P/drive_c/windows/syswow64/"
done
rm -rf "$tmp"
echo "installed DXVK $ver into $P"
