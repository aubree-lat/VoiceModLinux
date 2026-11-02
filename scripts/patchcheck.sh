#!/bin/bash
# patchcheck.sh <prefix> <build-dir>
#
# Starts Voicemod once with the vmwine hook injected, but WITHOUT creating any
# PipeWire devices, waits for the hook to finish its patching, prints the hook
# report plus the relevant lines of Voicemod's own log, then stops everything.
#
# Purpose: find out whether the in-memory patches still match the installed
# Voicemod build (they are byte patterns tied to one build).
set -u
P=${1:?usage: patchcheck.sh <prefix> <build-dir>}
BIN=${2:?usage: patchcheck.sh <prefix> <build-dir>}

export WINEPREFIX=$P WINEARCH=win64 WINEDEBUG=-all
export QTWEBENGINE_DISABLE_SANDBOX=1 QT_QUICK_BACKEND=software QT_OPENGL=software
export VMWINE_BLOCK_HOSTS=flagship-versions.voicemod.net   # never let it self-update
HOOKLOG=/tmp/vmwine-hook.log
WINELOG=${PATCHCHECK_WINELOG:-/tmp/vmwine-wine.log}

# keep the prefix's copies of the helpers in sync with the freshly built ones
sys=$P/drive_c/windows/system32
cmp -s "$BIN/vmhook.dll"   "$sys/vmhook.dll"   || cp "$BIN/vmhook.dll"   "$sys/vmhook.dll"
cmp -s "$BIN/vmlaunch.exe" "$sys/vmlaunch.exe" || cp "$BIN/vmlaunch.exe" "$sys/vmlaunch.exe"

VMLOG=$(ls "$P"/drive_c/users/*/AppData/Local/VoicemodV3/logs/voicemod-desktop.log 2>/dev/null | head -1)
before=$(grep -c "Voicemod app started" "$VMLOG" 2>/dev/null || echo 0)

rm -f "$HOOKLOG"
wineserver -k 2>/dev/null; sleep 1
wineserver -p </dev/null >/dev/null 2>&1
wine sc start voicemodvad >/dev/null 2>&1
wine 'C:\windows\system32\vmlaunch.exe' >"$WINELOG" 2>&1 &

# the hook logs "blockhosts" once after patching and once more 15 s later
for i in $(seq 1 40); do
    sleep 3
    [ "$(grep -c '^blockhosts' "$HOOKLOG" 2>/dev/null)" -ge 2 ] && break
done
sleep 10   # let the UI get as far as it will

echo "===== hook report ($HOOKLOG)"
cat "$HOOKLOG" 2>/dev/null || echo "(no hook log: hook never attached)"
echo
echo "===== Voicemod log, this session"
VMLOG=$(ls "$P"/drive_c/users/*/AppData/Local/VoicemodV3/logs/voicemod-desktop.log 2>/dev/null | head -1)
awk '/Voicemod app started/{buf=""} {buf=buf"\n"$0} END{print buf}' "$VMLOG" 2>/dev/null |
    grep -aiE 'version|BootPhase|Rendering info|Failed to share|driver|vad|exception|error|fail|usdk' | head -60
echo
echo "===== sessions started: before=$before now=$(grep -c 'Voicemod app started' "$VMLOG" 2>/dev/null)"
echo "===== processes"
pgrep -af 'Voicemod.exe' | sed 's/^/  /' | head -5

wineserver -k 2>/dev/null
echo "===== stopped"
