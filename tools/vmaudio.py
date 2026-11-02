#!/usr/bin/env python3
"""vmaudio.py -- keeps Voicemod's PipeWire wiring correct while the engine runs.

    vmaudio.py once  [--mic SOURCE]     fix the wiring now, then exit
    vmaudio.py watch [--mic SOURCE]     fix it, and keep fixing it until stopped

The wiring is  Voicemod:output ──> voicemod_out ──monitor──> voicemod_mic  <- apps pick this.
Nothing else may feed voicemod_mic: WirePlumber sometimes auto-links a stray source into a
virtual source, which would broadcast your desktop audio (or your unprocessed voice) down the
microphone. This watcher removes such links and restores the intended one.

Voicemod's *input* is NOT touched by default. Wine lists output monitors as selectable
"microphones" and Voicemod's first pick can be one of them, so the user must choose the real
mic in Voicemod (Settings > Audio > Input). Forcing it from outside fights the engine's own
watchdog, which restarts any stream that stays silent. `--relink-input` exists for debugging.

Routing ideas (check the mix sink's monitor exists, repair links until two clean passes,
never trust a fallback) come from aubree-lat/linuxpad's audio.py. Stdlib only; run with -I.
"""
import argparse
import signal
import subprocess
import sys
import time

VM_NODE = "Voicemod"            # the Wine process' stream node (its node.name)
OUT_SINK = "voicemod_out"       # what Voicemod writes to
MIC = "voicemod_mic"            # what Discord/OBS/games record from
CHANNELS = ("FL", "FR")


def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True, check=False).stdout


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def default_source():
    return run("pactl", "get-default-source").strip()


def real_sources():
    """Capture devices that are neither monitors nor ours."""
    out = []
    for line in run("pactl", "list", "short", "sources").splitlines():
        parts = line.split("\t")
        if len(parts) > 1 and not parts[1].endswith(".monitor") and not parts[1].startswith("voicemod"):
            out.append(parts[1])
    return out


def pick_mic(explicit, previous):
    """The explicit choice, else the system default input, else the last good one."""
    if explicit:
        return explicit
    cur = default_source()
    if cur and not cur.startswith("voicemod") and not cur.endswith(".monitor"):
        return cur
    if previous:
        return previous
    sources = real_sources()
    return sources[0] if sources else ""


def links():
    """Set of (output_port, input_port) from `pw-link -l` (every link is printed twice)."""
    pairs, cur = set(), None
    for raw in run("pw-link", "-l").splitlines():
        if not raw.startswith((" ", "\t")):
            cur = raw.strip()
            continue
        line = raw.strip()
        if line.startswith("|<-") and cur:
            pairs.add((line[3:].strip(), cur))
        elif line.startswith("|->") and cur:
            pairs.add((cur, line[3:].strip()))
    return pairs


def ports(flag):
    return {line.strip() for line in run("pw-link", flag).splitlines() if line.strip()}


def link(out_port, in_port):
    subprocess.run(["pw-link", out_port, in_port], capture_output=True, check=False)


def unlink(out_port, in_port):
    subprocess.run(["pw-link", "-d", out_port, in_port], capture_output=True, check=False)


def mic_ports_for(mic, outputs):
    mine = sorted(p for p in outputs if p.startswith(mic + ":"))
    pick = {}
    for ch in CHANNELS:
        match = next((p for p in mine if p.endswith("_" + ch)), None)
        pick[ch] = match or (mine[0] if mine else None)     # mono mic -> both channels
    return pick


def fix(mic, relink_input=False):
    """One repair pass. Returns a list of what was changed."""
    changed = []
    outputs, inputs, current = ports("-o"), ports("-i"), links()

    # 1. (debug only) Voicemod's capture stream: only the real mic may feed it.
    if mic and relink_input:
        want = mic_ports_for(mic, outputs)
        for ch in CHANNELS:
            inp = f"{VM_NODE}:input_{ch}"
            if inp not in inputs or not want[ch]:
                continue
            for out_port, in_port in current:
                if in_port == inp and out_port != want[ch]:
                    unlink(out_port, in_port)
                    changed.append(f"unlinked {out_port} -> {inp}")
            if (want[ch], inp) not in current:
                link(want[ch], inp)
                changed.append(f"linked {want[ch]} -> {inp}")

    # 2. The virtual mic: fed by Voicemod's sink monitor and nothing else.
    for ch in CHANNELS:
        src, dst = f"{OUT_SINK}:monitor_{ch}", f"{MIC}:input_{ch}"
        if dst not in inputs or src not in outputs:
            continue
        for out_port, in_port in current:
            if in_port == dst and out_port != src:
                unlink(out_port, in_port)
                changed.append(f"unlinked stray {out_port} -> {dst}")
        if (src, dst) not in current:
            link(src, dst)
            changed.append(f"linked {src} -> {dst}")
    return changed


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mode", choices=("once", "watch"))
    ap.add_argument("--mic", default="", help="PipeWire source name (default: follow the system default input)")
    ap.add_argument("--interval", type=float, default=1.5)
    ap.add_argument("--relink-input", action="store_true", help="debug: force Voicemod's input onto --mic")
    args = ap.parse_args()

    stop = False

    def _stop(*_):
        nonlocal stop
        stop = True

    signal.signal(signal.SIGTERM, _stop)
    signal.signal(signal.SIGINT, _stop)

    mic = ""
    last_mic = None
    while not stop:
        mic = pick_mic(args.mic, mic)
        if mic != last_mic:
            log(f"microphone: {mic or '(none found)'}")
            last_mic = mic
        for change in fix(mic, args.relink_input):
            log(change)
        if args.mode == "once":
            return 0
        time.sleep(args.interval)
    return 0


if __name__ == "__main__":
    sys.exit(main())
