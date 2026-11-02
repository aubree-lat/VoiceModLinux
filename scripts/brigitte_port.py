#!/usr/bin/env python3
"""brigitte_port.py <prefix> -- print the 127.0.0.1 TCP LISTEN ports owned by the
wineserver of this prefix (Voicemod's engine WebSocket "Brigitte" and the fixed
Control API port 59129). Stdlib only; run with -I.

Wine sockets belong to wineserver, so we map wineserver's socket inodes to
/proc/net/tcp LISTEN entries.
"""
import os
import re
import sys

CONTROL_API_PORT = 59129


def wineserver_pids(prefix):
    want = os.path.realpath(prefix)
    for pid in filter(str.isdigit, os.listdir("/proc")):
        try:
            if open(f"/proc/{pid}/comm").read().strip() != "wineserver":
                continue
            env = open(f"/proc/{pid}/environ", "rb").read().split(b"\0")
            pfx = next((e[11:].decode() for e in env if e.startswith(b"WINEPREFIX=")), None)
            if pfx is None or os.path.realpath(pfx) == want:
                yield pid
        except OSError:
            continue


def socket_inodes(pid):
    out = set()
    try:
        for fd in os.listdir(f"/proc/{pid}/fd"):
            try:
                m = re.fullmatch(r"socket:\[(\d+)\]", os.readlink(f"/proc/{pid}/fd/{fd}"))
                if m:
                    out.add(m.group(1))
            except OSError:
                pass
    except OSError:
        pass
    return out


def main():
    prefix = sys.argv[1]
    inodes = set()
    for pid in wineserver_pids(prefix):
        inodes |= socket_inodes(pid)
    for line in open("/proc/net/tcp").read().splitlines()[1:]:
        f = line.split()
        if len(f) < 10 or f[3] != "0A" or f[9] not in inodes:
            continue
        addr, port = f[1].split(":")
        port = int(port, 16)
        if addr == "0100007F":
            print(port, "(control api)" if port == CONTROL_API_PORT else "(candidate brigitte)")


if __name__ == "__main__":
    main()
