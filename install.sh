#!/bin/bash
# Voicemod for Linux: one-step setup.
#
#   ./install.sh            set everything up (asks before anything that needs your password)
#   ./install.sh --check    only report what is missing; changes nothing
#   ./install.sh --yes      don't ask questions (for scripts)
#
# Made for Fedora Workstation; Arch/CachyOS and Ubuntu/Mint/Debian are supported too.
# Not affiliated with Voicemod S.L.: you need your own Voicemod account and installer.
set -u

HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
# A zip download can lose the "executable" flag; put it back.
chmod +x "$HERE/vmwine" "$HERE/uninstall.sh" "$HERE"/tools/* "$HERE"/scripts/*.sh 2>/dev/null
ELECTRON_VERSION=44.9.1          # the version this project was tested with
MIN_WINE=11                      # tested on Wine 11.x
CHECK_ONLY=0
ASSUME_YES=0
for arg in "$@"; do
    case $arg in
        --check) CHECK_ONLY=1 ;;
        --yes|-y) ASSUME_YES=1 ;;
        -h|--help) sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $arg (try --help)"; exit 2 ;;
    esac
done

say()  { printf '\n\033[1m== %s\033[0m\n' "$*"; }
note() { printf '   %s\n' "$*"; }
ok()   { printf '   \033[32mok\033[0m  %s\n' "$*"; }
bad()  { printf '   \033[31mMISSING\033[0m  %s\n' "$*"; }
die()  { printf '\n\033[31mStopped: %s\033[0m\n' "$*" >&2; exit 1; }
ask()  {   # ask "question"  -> 0 for yes
    [ "$ASSUME_YES" = 1 ] && return 0
    local r; read -r -p "   $1 [Y/n] " r; [[ -z $r || $r == [yY]* ]]
}

[ "$(id -u)" = 0 ] && die "please run this as your normal user, not with sudo. It asks for your password only when needed."

# ---------------------------------------------------------------- which Linux is this?
say "Checking your system"
. /etc/os-release 2>/dev/null
FAMILY=unknown
case " ${ID:-} ${ID_LIKE:-} " in
    *" fedora "*)              FAMILY=fedora ;;
    *" arch "*)                FAMILY=arch ;;
    *" debian "*|*" ubuntu "*) FAMILY=debian ;;
esac
note "${PRETTY_NAME:-unknown system} (${FAMILY})"
[ "$FAMILY" = unknown ] && die "I don't know this Linux yet. Supported: Fedora, Arch/CachyOS, Ubuntu/Mint/Debian."

case $FAMILY in
    fedora)
        PKGS="wine mingw64-gcc make pipewire-utils pulseaudio-utils python3 python3-evdev nodejs npm curl unzip rsync xdg-utils"
        INSTALL="sudo dnf install -y" ;;
    arch)
        PKGS="wine mingw-w64-gcc make pipewire-pulse libpulse python python-evdev nodejs npm curl unzip rsync xdg-utils"
        INSTALL="sudo pacman -S --needed" ;;
    debian)
        PKGS="gcc-mingw-w64-x86-64 make pulseaudio-utils pipewire-bin python3 python3-evdev nodejs npm curl unzip rsync xdg-utils"
        INSTALL="sudo apt install" ;;
esac

# ---------------------------------------------------------------- what is missing?
MISSING=0
need_cmd() { if command -v "$1" >/dev/null 2>&1; then ok "$1"; else bad "$1  ($2)"; MISSING=1; fi; }
need_cmd wine                    "runs the Windows part of Voicemod"
need_cmd x86_64-w64-mingw32-gcc  "builds the small Windows helpers"
need_cmd make                    "runs the build"
need_cmd pactl                   "creates the virtual microphone"
need_cmd pw-link                 "wires the virtual microphone"
need_cmd python3                 ""
need_cmd node                    "runs the Voicemod window"
need_cmd npm                     "downloads the window runtime"
need_cmd curl                    ""
need_cmd unzip                   ""
need_cmd rsync                   ""
if python3 -c 'import evdev' 2>/dev/null; then ok "python evdev (hotkeys)"; else bad "python evdev  (hotkeys)"; MISSING=1; fi

if [ "$MISSING" = 1 ]; then
    say "Some programs are missing"
    note "They come from your system's normal software sources. The command is:"
    printf '\n      %s %s\n\n' "$INSTALL" "$PKGS"
    if [ "$CHECK_ONLY" = 1 ]; then
        note "(--check: not installing anything)"
    elif ask "Run that now? It will ask for your password."; then
        # shellcheck disable=SC2086
        $INSTALL $PKGS || die "the install command failed. Fix the message above and run ./install.sh again."
    else
        die "install those packages, then run ./install.sh again."
    fi
fi
[ "$CHECK_ONLY" = 1 ] && MISSING_AFTER=$MISSING

# ---------------------------------------------------------------- Wine must be new enough
if command -v wine >/dev/null 2>&1; then
    WINE_VER=$(wine --version 2>/dev/null | sed -n 's/^wine-\([0-9]*\).*/\1/p')
    if [ -n "$WINE_VER" ] && [ "$WINE_VER" -lt "$MIN_WINE" ]; then
        bad "Wine $WINE_VER is too old (need $MIN_WINE or newer)"
        case $FAMILY in
            debian) note "Ubuntu/Mint ship an old Wine. Add WineHQ's repository, then install 'winehq-stable':"
                    note "https://wiki.winehq.org/Ubuntu" ;;
            *)      note "Update your system, then run this again." ;;
        esac
        [ "$CHECK_ONLY" = 1 ] && MISSING_AFTER=1 || die "Wine is too old."
    elif [ -n "$WINE_VER" ]; then
        ok "Wine $WINE_VER is new enough"
    fi
fi

if [ "$CHECK_ONLY" = 1 ]; then
    say "Check finished"
    [ "${MISSING_AFTER:-0}" = 0 ] && note "Everything needed is here. Run ./install.sh to set up." || note "Run ./install.sh and it will fix the list above."
    exit 0
fi

# ---------------------------------------------------------------- build + window runtime
say "Building the small Windows helpers"
make -C "$HERE" >/dev/null 2>"$HERE/build.log" || {
    tail -5 "$HERE/build.log"
    die "the build failed (details in build.log). Please send that file to whoever gave you this."
}
ok "built"

say "Getting the Voicemod window runtime (Electron $ELECTRON_VERSION)"
if command -v electron >/dev/null 2>&1; then
    ok "using the system's electron"
elif [ -x "$HERE/cache/electron/node_modules/.bin/electron" ]; then
    ok "already downloaded"
else
    mkdir -p "$HERE/cache/electron"
    npm install --prefix "$HERE/cache/electron" --no-audit --no-fund --save-exact "electron@$ELECTRON_VERSION" >/dev/null 2>&1 ||
        die "could not download Electron. Check your internet connection and run ./install.sh again."
    ok "downloaded"
fi

# ---------------------------------------------------------------- Voicemod's own installer
say "Finding your Voicemod installer"
find_installer() {
    local d f
    for d in "$HERE/installer" "$HOME/Downloads" "$HOME/Desktop" "$HOME"; do
        [ -d "$d" ] || continue
        f=$(ls -t "$d"/VoicemodInstaller*.exe 2>/dev/null | head -1)
        [ -n "$f" ] && { echo "$f"; return; }
    done
}
INSTALLER=$(find_installer)
if [ -z "$INSTALLER" ]; then
    note "I couldn't find the Voicemod installer (a file called VoicemodInstaller_....exe)."
    note "You download it yourself from Voicemod, so it stays your own copy:"
    note "  1. Open https://www.voicemod.net/ and sign in (or make a free account)"
    note "  2. Download the Windows installer. Your browser saves it to Downloads."
    command -v xdg-open >/dev/null && xdg-open "https://www.voicemod.net/" >/dev/null 2>&1 &
    if [ "$ASSUME_YES" = 1 ]; then die "no installer found."; fi
    read -r -p "   Press Enter when the download has finished... " _
    INSTALLER=$(find_installer)
    [ -n "$INSTALLER" ] || die "still no VoicemodInstaller*.exe in Downloads. Download it, then run ./install.sh again."
fi
ok "$INSTALLER"

say "Installing Voicemod (this downloads about 400 MB and takes several minutes)"
note "Windows may flash a few small windows. That's normal. Please wait."
"$HERE/vmwine" install "$INSTALLER" || die "the Voicemod install failed. See the messages above."

# ---------------------------------------------------------------- convenience + hotkeys
say "Finishing up"
mkdir -p "$HOME/.local/bin"
ln -sf "$HERE/vmwine" "$HOME/.local/bin/vmwine"
ok "'vmwine' command added (new terminals can use it)"

if id -nG | tr ' ' '\n' | grep -qx input; then
    ok "hotkeys: your account can already read the keyboard"
else
    note "Hotkeys (keys that work in any program) need permission to read the keyboard."
    note "This adds you to the 'input' group. It does not give anyone else access."
    if ask "Allow that? It asks for your password."; then
        sudo usermod -aG input "$USER" && note "Done. You must LOG OUT AND BACK IN once for it to take effect."
    else
        note "Skipped. Voicemod works; only global hotkeys won't. Re-run ./install.sh to add it later."
    fi
fi

say "All done"
note "Start Voicemod: open your app menu and search for \"Voicemod\", or run:  vmwine"
note "First start takes about a minute, then:"
note "  1. Click 'Get Started' and sign in. Your browser opens for the login."
note "  2. Open Settings > Audio and set the INPUT to your real microphone"
note "     (not 'PulseAudio Input' and not anything saying 'Monitor of')."
note "  3. In Discord, games or OBS, choose the microphone called 'Voicemod'."
note "More help: README.md"
