#!/bin/bash
# trial.sh <dxvk-tag>...
# For each DXVK release: install it into the project prefix, start Voicemod via
# vmwine (full flow, with the PipeWire devices), and print a one-line verdict.
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
P=$HERE/prefix
D=$(ls -d "$P"/drive_c/users/*/AppData/Local/Temp/.ui-agent 2>/dev/null | head -1)
VMLOG=$(ls "$P"/drive_c/users/*/AppData/Local/VoicemodV3/logs/voicemod-desktop.log | head -1)

for tag in "$@"; do
    "$HERE/scripts/dxvk-swap.sh" "$P" "$tag" >/dev/null 2>&1 || { echo "RESULT $tag: swap FAILED"; continue; }
    before=$(cat "$D/last_crash" 2>/dev/null)
    VMWINE_PREFIX=$P VMWINE_LOG=$HERE/trial-$tag-wine.log "$HERE/vmwine" run >"$HERE/trial-$tag.log" 2>&1
    rc=$?
    after=$(cat "$D/last_crash" 2>/dev/null)
    sess=$(awk '/Voicemod app started/{buf=""} {buf=buf"\n"$0} END{print buf}' "$VMLOG")
    share=$(grep -ac 'Failed to share D3D11' <<<"$sess")
    render=$(grep -ac 'Rendering info' <<<"$sess")
    alive=$(pgrep -f '[a]pp.last.Voicemod.exe' >/dev/null && echo yes || echo no)
    crashed=$([ "$before" != "$after" ] && echo yes || echo no)
    echo "RESULT $tag: verified=$([ $rc = 0 ] && echo yes || echo no) crashed=$crashed alive=$alive share_fail_lines=$share rendering_info=$render"
done
