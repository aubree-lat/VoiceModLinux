#!/usr/bin/env python3
"""vmkeys.py -- global hotkeys for Voicemod on Wayland (niri, Hyprland, GNOME, KDE...).

Voicemod's own hotkeys can't see your keyboard under Wine on Wayland, so this reads the
keyboards through /dev/input (like aubree-lat/linuxpad's hotkeys.py) and sends the matching
command to the running app's control socket (the one `voicemod-ctl` uses).

Two sources of bindings:
  * the keybinds you made INSIDE Voicemod: the app copies them out of the engine every few
    seconds ($XDG_RUNTIME_DIR/vmwine-voicemod-binds.json) and they are mirrored automatically;
  * your own file, ~/.config/vmwine/hotkeys.conf (these win over a mirrored bind on the same keys).

    vmkeys.py run [--conf FILE]       listen until stopped
    vmkeys.py check [--conf FILE]     show every binding, what could not be mirrored, key access
    vmkeys.py init [--conf FILE]      write an example config if none exists
    vmkeys.py add KEY "command"       add or replace one binding, e.g.  add F9 "voice-changer toggle"
    vmkeys.py remove KEY              delete a binding

Matching: a bind fires when its keys are all held and the key just pressed is one of them.
If several binds match, only the most specific ones (most keys) fire, so holding W to walk
never blocks a K bind. Keys are read PASSIVELY (never grabbed) and still reach the focused
window. Needs the `input` group and python-evdev. Only bound keys are acted on; nothing is
logged or stored.

Config lines:   KEY = command   or   MOD+KEY = command   (MOD: ctrl, alt, shift, super)
Commands: voice NAME|random, sound NAME|stop, voice-changer on|off|toggle,
          hear-myself on|off|toggle, mute on|off|toggle, wheel toggle, show
"""
import argparse
import json
import os
import re
import select
import signal
import socket
import sys
import threading
import time
from pathlib import Path

try:
    import evdev
    from evdev import ecodes
except ImportError:
    evdev = ecodes = None

XDG_CONFIG = Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config")
DEFAULT_CONF = XDG_CONFIG / "vmwine" / "hotkeys.conf"
NO_SYNC = XDG_CONFIG / "vmwine" / "no-sync"            # present = do not mirror Voicemod's binds
RUNTIME = Path(os.environ.get("XDG_RUNTIME_DIR") or "/tmp")
SOCKET = RUNTIME / "voicemod-linux.sock"
SYNCED = RUNTIME / "vmwine-voicemod-binds.json"

MODS = {
    "ctrl": frozenset({"KEY_LEFTCTRL", "KEY_RIGHTCTRL"}),
    "alt": frozenset({"KEY_LEFTALT", "KEY_RIGHTALT"}),
    "shift": frozenset({"KEY_LEFTSHIFT", "KEY_RIGHTSHIFT"}),
    "super": frozenset({"KEY_LEFTMETA", "KEY_RIGHTMETA"}),
}
COMMANDS = ("voice", "sound", "voice-changer", "hear-myself", "mute", "wheel", "show")

EXAMPLE = """\
# Voicemod global hotkeys (read by vmkeys.py). One binding per line:
#   KEY = command        or        MOD+KEY = command      (MOD: ctrl, alt, shift, super)
# Binds you make inside Voicemod itself are mirrored automatically; this file is for extras
# and it wins over a mirrored bind on the same keys.
# Commands: voice NAME|random|list, sound NAME|stop|list, voice-changer on|off|toggle,
# hear-myself on|off|toggle, mute on|off|toggle, wheel toggle, show
# Keys are read passively (never grabbed) and still reach the focused window.
#
# F9       = voice-changer toggle
# ctrl+F10 = mute toggle
# KP1      = voice Banana
"""


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


# ---------------------------------------------------------------- key names / Qt codes

def key_name(raw):
    name = raw.strip().upper()
    return name if name.startswith("KEY_") else "KEY_" + name


QT_MOD_FLAGS = {0x02000000: "shift", 0x04000000: "ctrl", 0x08000000: "alt", 0x10000000: "super"}
QT_KEYPAD = 0x20000000
QT_MODKEYS = {0x20: "shift", 0x21: "ctrl", 0x22: "super", 0x23: "alt"}
QT_SPECIAL = {0x00: "ESC", 0x01: "TAB", 0x03: "BACKSPACE", 0x04: "ENTER", 0x05: "KPENTER",
              0x06: "INSERT", 0x07: "DELETE", 0x08: "PAUSE", 0x09: "SYSRQ", 0x10: "HOME",
              0x11: "END", 0x12: "LEFT", 0x13: "UP", 0x14: "RIGHT", 0x15: "DOWN",
              0x16: "PAGEUP", 0x17: "PAGEDOWN", 0x24: "CAPSLOCK", 0x25: "NUMLOCK",
              0x26: "SCROLLLOCK"}
ASCII_KEYS = {32: "SPACE", 39: "APOSTROPHE", 44: "COMMA", 45: "MINUS", 46: "DOT", 47: "SLASH",
              59: "SEMICOLON", 61: "EQUAL", 91: "LEFTBRACE", 92: "BACKSLASH",
              93: "RIGHTBRACE", 96: "GRAVE"}
KEYPAD_ASCII = {42: "KPASTERISK", 43: "KPPLUS", 45: "KPMINUS", 46: "KPDOT", 47: "KPSLASH"}
# Voicemod's own code for the Alt key. Confirmed by the user: 0x20002387 is Left Alt (their
# "Azure_swing" bind); 0x2387 is the Alt of Voicemod's default wheel hotkey Alt+V (either side).
VM_ALT = 0x2387


def qt_to_keys(code):
    """One Voicemod/Qt key code -> frozenset of acceptable evdev names, or None if unknown.
    Returns the string 'ignore' for a bare keypad flag (carries no key)."""
    flags, base = code & 0xFE000000, code & 0x01FFFFFF
    keypad = bool(flags & QT_KEYPAD)
    if base == 0:                                       # modifier-only entry (Shift, Ctrl...)
        mods = [m for f, m in QT_MOD_FLAGS.items() if flags & f]
        if len(mods) == 1:
            return MODS[mods[0]]
        return "ignore" if not mods else None
    if base == VM_ALT:
        return frozenset({"KEY_LEFTALT"}) if keypad else MODS["alt"]
    if base >= 0x01000000:                              # Qt::Key_* specials
        k = base - 0x01000000
        if k in QT_MODKEYS:
            return MODS[QT_MODKEYS[k]]
        if k in QT_SPECIAL:
            return frozenset({"KEY_" + QT_SPECIAL[k]})
        if 0x30 <= k <= 0x4B:                           # F1..F24
            return frozenset({f"KEY_F{k - 0x30 + 1}"})
        return None
    if 65 <= base <= 90:                                # letters
        return frozenset({"KEY_" + chr(base)})
    if 48 <= base <= 57:
        return frozenset({("KEY_KP" if keypad else "KEY_") + chr(base)})
    if keypad and base in KEYPAD_ASCII:
        return frozenset({"KEY_" + KEYPAD_ASCII[base]})
    if base in ASCII_KEYS:
        return frozenset({"KEY_" + ASCII_KEYS[base]})
    return None


# ---------------------------------------------------------------- bindings

class Bind:
    def __init__(self, req, cmd, source, label=""):
        self.req = tuple(req)                           # each element: frozenset of acceptable keys
        self.cmd, self.source, self.label = cmd, source, label
        self.universe = frozenset().union(*self.req) if self.req else frozenset()
        self.sig = tuple(sorted(tuple(sorted(s)) for s in self.req))

    def satisfied(self, held):
        return all(s & held for s in self.req)

    def describe(self):
        return " + ".join("/".join(k.removeprefix("KEY_") for k in sorted(s)) for s in self.req)


def parse_lhs(lhs):
    parts = [p.strip() for p in lhs.split("+") if p.strip()]
    mods = [m.lower() for m in parts[:-1]]
    return (key_name(parts[-1]) if parts else ""), mods


def parse_conf(path):
    """-> (list of Bind, [error strings])"""
    binds, errors = [], []
    if not path.exists():
        return binds, errors
    for n, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split("#", 1)[0].strip()
        if not line:
            continue
        if "=" not in line:
            errors.append(f"line {n}: expected 'KEY = command'")
            continue
        lhs, cmd = (x.strip() for x in line.split("=", 1))
        key, mods = parse_lhs(lhs)
        bad = set(mods) - set(MODS)
        if not key or not cmd:
            errors.append(f"line {n}: empty key or command")
        elif bad:
            errors.append(f"line {n}: unknown modifier {', '.join(sorted(bad))}")
        elif ecodes is not None and key not in ecodes.ecodes:
            errors.append(f"line {n}: unknown key {lhs!r}")
        else:
            binds.append(Bind([frozenset({key})] + [MODS[m] for m in mods], cmd, "manual", lhs))
    return binds, errors


def load_synced():
    """Voicemod's own binds as written by the app -> (list of Bind, unsupported [(label, why)])."""
    binds, unsupported = [], []
    if NO_SYNC.exists() or not SYNCED.exists():
        return binds, unsupported
    try:
        data = json.loads(SYNCED.read_text())
    except (OSError, ValueError):
        return binds, unsupported
    for b in data.get("binds", []):
        req, bad = [], []
        for code in b.get("keys", []):
            k = qt_to_keys(code)
            if k == "ignore":
                continue
            if k is None:
                bad.append(f"0x{code:x}")
            else:
                req.append(k)
        if bad:
            unsupported.append((b.get("label", b.get("cmd", "?")), "unknown key code " + ", ".join(bad)))
        elif req:
            binds.append(Bind(req, b["cmd"], "voicemod", b.get("label", "")))
    for s in data.get("skipped", []):
        unsupported.append((s.get("label", s.get("action", "?")), s.get("reason", "not supported")))
    return binds, unsupported


def add_binding(conf, lhs, command):
    key, mods = parse_lhs(lhs)
    if not key or (ecodes is not None and key not in ecodes.ecodes):
        sys.exit(f"vmkeys: '{lhs}' is not a key I know (examples: F9, KP1, ctrl+F10, alt+K)")
    if set(mods) - set(MODS):
        sys.exit("vmkeys: modifiers can be ctrl, alt, shift, super")
    if command.split()[0] not in COMMANDS:
        sys.exit("vmkeys: the command must start with one of: " + ", ".join(COMMANDS))
    canon = "+".join(mods + [key.removeprefix("KEY_")])
    conf.parent.mkdir(parents=True, exist_ok=True)
    lines = conf.read_text().splitlines() if conf.exists() else EXAMPLE.splitlines()
    out = []
    for line in lines:
        body = line.split("#", 1)[0]
        if "=" in body and parse_lhs(body.split("=", 1)[0]) == (key, mods):
            continue
        out.append(line)
    out.append(f"{canon} = {command}")
    conf.write_text("\n".join(out) + "\n")
    print(f"saved: {canon} -> {command}")
    if not mods and len(key) == 5:                      # KEY_x: a single letter/digit
        print(f"NOTE: '{canon}' fires every time you type that key in ANY app. Safer: F-keys, the "
              f"number pad, or a combination like alt+{canon}.")


def remove_binding(conf, lhs):
    target = parse_lhs(lhs)
    kept, removed = [], 0
    for line in (conf.read_text().splitlines() if conf.exists() else []):
        body = line.split("#", 1)[0]
        if "=" in body and parse_lhs(body.split("=", 1)[0]) == target:
            removed += 1
        else:
            kept.append(line)
    conf.write_text("\n".join(kept) + "\n")
    print(f"removed {removed} binding(s)")


# ---------------------------------------------------------------- devices / sending

def keyboards():
    """Real typing keyboards: both the kbd and leds handlers (macro-pad/mouse nodes lack leds)."""
    out = []
    try:
        blob = Path("/proc/bus/input/devices").read_text()
    except OSError:
        return out
    for block in blob.split("\n\n"):
        name = re.search(r'Name="(.*)"', block)
        handlers = re.search(r"Handlers=(.*)", block)
        if not handlers:
            continue
        h = handlers.group(1).split()
        ev = next((x for x in h if x.startswith("event")), None)
        if "kbd" in h and "leds" in h and ev:
            node = f"/dev/input/{ev}"
            out.append((name.group(1) if name else ev, node, os.access(node, os.R_OK)))
    return out


def send(command):
    try:
        with socket.socket(socket.AF_UNIX) as s:
            s.settimeout(6)
            s.connect(str(SOCKET))
            s.sendall((command + "\n").encode())
            return s.makefile().readline().strip()
    except OSError as err:
        return f"error: cannot reach Voicemod ({err})"


def fire(bind):
    def work():
        log(f"{bind.source}: {bind.label or bind.cmd} -> {send(bind.cmd)}")
    threading.Thread(target=work, daemon=True).start()


def choose(binds, name, held):
    """The binds to fire for a key-down of `name` with `held` keys down."""
    cands = [b for b in binds if name in b.universe and b.satisfied(held)]
    if not cands:
        return []
    top = max(len(b.req) for b in cands)
    chosen = {}
    for b in cands:
        if len(b.req) != top:
            continue
        cur = chosen.get(b.sig)
        if cur is None or (b.source == "manual" and cur.source != "manual"):
            chosen[b.sig] = b                           # a hand-written bind beats a mirrored one
    return list(chosen.values())


def run(conf):
    if evdev is None:
        sys.exit("vmkeys: python-evdev is not installed (Arch: pacman -S python-evdev)")
    binds, stamp = [], None
    devices, held, last_fired = {}, set(), {}
    stop = False

    def _stop(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)
    last_scan = 0.0

    def mtime(p):
        try:
            return p.stat().st_mtime
        except OSError:
            return None

    while not stop:
        now = time.monotonic()
        if now - last_scan > 3:                         # hot-plug + reload of both sources
            last_scan = now
            new = (mtime(conf), mtime(SYNCED), NO_SYNC.exists())
            if new != stamp:
                stamp = new
                manual, errors = parse_conf(conf)
                synced, unsupported = load_synced()
                binds = manual + synced
                for e in errors:
                    log(f"{conf}: {e}")
                log(f"{len(manual)} own + {len(synced)} mirrored binding(s) loaded"
                    + (f", {len(unsupported)} not supported" if unsupported else ""))
            for name, node, ok in keyboards():
                if ok and node not in devices:
                    try:
                        devices[node] = evdev.InputDevice(node)
                        log(f"listening on {name} ({node})")
                    except OSError:
                        pass
        if not devices:
            time.sleep(1)
            continue
        ready, _, _ = select.select([d.fd for d in devices.values()], [], [], 1.0)
        for node, dev in list(devices.items()):
            if dev.fd not in ready:
                continue
            try:
                events = list(dev.read())
            except OSError:                             # unplugged
                devices.pop(node, None)
                continue
            for ev in events:
                if ev.type != ecodes.EV_KEY:
                    continue
                name = ecodes.KEY.get(ev.code)
                name = name[0] if isinstance(name, (list, tuple)) else name
                if not name:
                    continue
                if ev.value == 0:
                    held.discard(name)
                elif ev.value == 1:
                    held.add(name)
                    for b in choose(binds, name, held):
                        t = time.monotonic()            # several nodes may report one press
                        if t - last_fired.get(b.sig, 0.0) > 0.25:
                            last_fired[b.sig] = t
                            fire(b)
    return 0


def show(conf):
    manual, errors = parse_conf(conf)
    synced, unsupported = load_synced()
    print(f"config: {conf} ({'found' if conf.exists() else 'missing'})")
    print(f"mirroring Voicemod's binds: {'OFF (vmwine hotkeys sync on)' if NO_SYNC.exists() else 'on'}"
          f"{'' if SYNCED.exists() else ' (no data yet: start the app)'}")
    print(f"\nyour own binds ({len(manual)}):")
    for b in manual:
        print(f"  {b.describe():28} -> {b.cmd}")
    for e in errors:
        print("  error:", e)
    print(f"\nmirrored from Voicemod ({len(synced)}):")
    for b in sorted(synced, key=lambda b: (len(b.req), b.describe())):
        print(f"  {b.describe():28} -> {b.label or b.cmd}")
    if unsupported:
        print(f"\nnot supported yet ({len(unsupported)}):")
        for label, why in unsupported:
            print(f"  {label}  ({why})")
    print("\npython-evdev:", "ok" if evdev else "MISSING")
    for name, node, ok in keyboards():
        print(f"  keyboard {name} {node}: {'readable' if ok else 'NOT readable (is your user in the input group?)'}")
    print("control socket:", SOCKET, "(app running)" if SOCKET.exists() else "(app not running)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=("run", "check", "init", "add", "remove"))
    ap.add_argument("args", nargs="*", help="add KEY COMMAND... | remove KEY")
    ap.add_argument("--conf", type=Path, default=Path(os.environ.get("VMWINE_HOTKEYS") or DEFAULT_CONF))
    a = ap.parse_args()

    if a.mode == "add":
        if len(a.args) < 2:
            sys.exit('usage: vmkeys.py add KEY "command"   e.g.  add F9 "voice-changer toggle"')
        add_binding(a.conf, a.args[0], " ".join(a.args[1:]))
        return 0
    if a.mode == "remove":
        if not a.args:
            sys.exit("usage: vmkeys.py remove KEY")
        remove_binding(a.conf, a.args[0])
        return 0
    if a.mode == "init":
        if a.conf.exists():
            print(f"{a.conf} already exists")
        else:
            a.conf.parent.mkdir(parents=True, exist_ok=True)
            a.conf.write_text(EXAMPLE)
            print(f"wrote {a.conf}")
        return 0
    if a.mode == "check":
        show(a.conf)
        return 0
    return run(a.conf)


if __name__ == "__main__":
    sys.exit(main())
