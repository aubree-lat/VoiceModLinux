#!/bin/bash
# Voicemod for Linux: one-paste installer.
#
#   curl -fsSL https://raw.githubusercontent.com/aubree-lat/VoiceModLinux/main/get.sh | bash
#
# What it does, and nothing more:
#   1. downloads this project (a pinned release, see REF) from GitHub into
#      ~/.local/share/voicemod-linux
#   2. runs its install.sh, which explains each step and asks before anything that needs
#      your password
# Re-running it updates the project and keeps your Voicemod setup.
#
# Options are passed on to install.sh, e.g.  ... | bash -s -- --check
# Overrides (for testing): VMLINUX_REF, VMLINUX_HOME, VMLINUX_TARBALL
set -eu

REPO=aubree-lat/VoiceModLinux
REF=${VMLINUX_REF:-v0.1.0}                         # a release tag; bump it when you publish a new one
DEST=${VMLINUX_HOME:-${XDG_DATA_HOME:-$HOME/.local/share}/voicemod-linux}
URL=${VMLINUX_TARBALL:-https://github.com/$REPO/archive/$REF.tar.gz}

die() { printf '\n\033[31mStopped: %s\033[0m\n' "$*" >&2; exit 1; }

[ "$(id -u)" = 0 ] && die "please run this as your normal user, not as root or with sudo."
for tool in curl tar mktemp; do
    command -v "$tool" >/dev/null 2>&1 || die "'$tool' is missing. Install it with your system's software manager and try again."
done

printf '\n\033[1m== Voicemod for Linux (%s)\033[0m\n' "$REF"
printf '   Downloading from github.com/%s into:\n   %s\n\n' "$REPO" "$DEST"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

curl -fSL --progress-bar "$URL" -o "$TMP/project.tar.gz" ||
    die "the download failed. Check your internet connection, then paste the command again."
mkdir "$TMP/src"
tar -xzf "$TMP/project.tar.gz" -C "$TMP/src" --strip-components=1 ||
    die "the downloaded file is damaged. Paste the command again."
[ -f "$TMP/src/install.sh" ] || die "the download doesn't look like Voicemod for Linux (no install.sh)."

# Update in place: replace our own files, keep what the installer already fetched.
mkdir -p "$DEST"
rm -rf "$DEST/src" "$DEST/tools" "$DEST/ui" "$DEST/scripts" "$DEST/third_party"
cp -a "$TMP/src/." "$DEST/"
chmod +x "$DEST/install.sh" "$DEST/uninstall.sh" "$DEST/vmwine" 2>/dev/null || true

cd "$DEST"
# We arrive through a pipe, so the installer's questions must read from the keyboard.
if (exec </dev/tty) 2>/dev/null; then
    exec bash install.sh "$@" </dev/tty
else
    exec bash install.sh "$@"
fi
