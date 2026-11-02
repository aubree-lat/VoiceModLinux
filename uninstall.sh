#!/bin/bash
# Removes Voicemod for Linux: the Wine prefix (Voicemod and its settings), the menu entries,
# the virtual microphone and the 'vmwine' command. Your Voicemod ACCOUNT is not affected.
# This project's own folder is left alone; delete it yourself if you want it gone too.
set -u
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)

read -r -p "Remove Voicemod and all its local settings? [y/N] " r
[[ $r == [yY]* ]] || { echo "Nothing changed."; exit 0; }

"$HERE/vmwine" stop 2>/dev/null
"$HERE/vmwine" uninstall --yes 2>/dev/null
rm -f "$HOME/.local/share/applications/vmwine-url.desktop" \
      "$HOME/.local/share/applications/vmwine.desktop" \
      "$HOME/.local/bin/vmwine"
command -v update-desktop-database >/dev/null && update-desktop-database "$HOME/.local/share/applications" 2>/dev/null
echo "Removed. Your hotkey settings (~/.config/vmwine) were kept; delete that folder if you like."
