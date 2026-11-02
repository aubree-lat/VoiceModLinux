#!/usr/bin/env python3
"""minidump_info.py <file.dmp> -- print the exception record and the module the
faulting address falls in. Minimal MINIDUMP parser (stdlib only); run with -I.

Streams used: 4 ModuleList, 6 ExceptionStream, 3 ThreadList (id only), 7 SystemInfo.
"""
import struct
import sys


def u32(b, o): return struct.unpack_from("<I", b, o)[0]
def u64(b, o): return struct.unpack_from("<Q", b, o)[0]


def read_utf16(b, rva):
    n = u32(b, rva)
    return b[rva + 4: rva + 4 + n].decode("utf-16-le", "replace")


def main(path):
    b = open(path, "rb").read()
    if b[:4] != b"MDMP":
        sys.exit("not a minidump")
    nstreams, dir_rva = u32(b, 8), u32(b, 12)
    streams = {}
    for i in range(nstreams):
        t, size, rva = struct.unpack_from("<III", b, dir_rva + 12 * i)
        streams[t] = (size, rva)
    print("streams:", sorted(streams))

    mods = []
    if 4 in streams:
        _, rva = streams[4]
        for i in range(u32(b, rva)):
            o = rva + 4 + 108 * i
            base, size = u64(b, o), u32(b, o + 8)
            mods.append((base, size, read_utf16(b, u32(b, o + 20))))

    if 7 in streams:
        _, rva = streams[7]
        print("os: arch=%d major=%d minor=%d build=%d" % (
            struct.unpack_from("<H", b, rva)[0], u32(b, rva + 8), u32(b, rva + 12), u32(b, rva + 16)))

    if 6 not in streams:
        print("no exception stream (a requested dump, not a crash)")
    else:
        _, rva = streams[6]
        tid = u32(b, rva)
        code, flags = u32(b, rva + 8), u32(b, rva + 12)
        addr, nparams = u64(b, rva + 24), u32(b, rva + 32)
        params = [u64(b, rva + 40 + 8 * i) for i in range(min(nparams, 15))]
        print("exception: thread=%d code=0x%08x flags=0x%x address=0x%x params=%s" % (
            tid, code, flags, addr, [hex(p) for p in params]))
        for base, size, name in mods:
            if base <= addr < base + size:
                print("  faulting address is in %s +0x%x" % (name, addr - base))
                break
        else:
            print("  faulting address is not inside any listed module")

        # --- what was thrown? (libgcc C++ exception: params[0] = &_Unwind_Exception) ---
        mem = []                                    # (start, size, rva) from MemoryList
        if 5 in streams:
            _, mrva = streams[5]
            for i in range(u32(b, mrva)):
                o = mrva + 4 + 16 * i
                mem.append((u64(b, o), u32(b, o + 8), u32(b, o + 12)))

        def rd(a, n):
            for s, sz, r in mem:
                if s <= a and a + n <= s + sz:
                    return b[r + (a - s): r + (a - s) + n]
            return None

        def cstr(a, limit=200):
            out = b""
            while len(out) < limit:
                c = rd(a + len(out), 1)
                if not c or c == b"\0":
                    break
                out += c
            return out.decode("latin-1")

        if code == 0x20474343 and params:
            hdr = params[0]
            ti = rd(hdr - 80, 8)                    # __cxa_exception.exceptionType
            if ti:
                tinfo = struct.unpack("<Q", ti)[0]
                nm = rd(tinfo + 8, 8)
                if nm:
                    print("  thrown C++ type (mangled): %s" % cstr(struct.unpack("<Q", nm)[0]))
            obj = rd(hdr, 0x40)                     # the object itself starts right after the header
            obj = rd(hdr + 32, 0x80)                # (unwindHeader is 32 bytes; object follows)
            if obj:
                # dump any printable runs: std::string payloads (DxvkError message) show up here
                import re
                for m in re.finditer(rb"[ -~]{6,}", obj):
                    print("  object text: %r" % m.group().decode())
                # follow pointers once: an inline-less std::string stores a pointer to its text
                for k in range(0, len(obj) - 7, 8):
                    p = struct.unpack_from("<Q", obj, k)[0]
                    t = rd(p, 8) if p > 0x10000 else None
                    if t:
                        s = cstr(p, 160)
                        if len(s) >= 6 and all(32 <= ord(ch) < 127 for ch in s):
                            print("  object ptr+%d -> %r" % (k, s))
        # poor-man's backtrace: scan the faulting thread's stack for qwords that
        # point into a module (return addresses), and print module+offset.
        ctx_size, ctx_rva = u32(b, rva + 160), u32(b, rva + 164)
        rsp = u64(b, ctx_rva + 0x98) if ctx_size >= 0xF8 else 0
        rip = u64(b, ctx_rva + 0xF8) if ctx_size >= 0x100 else 0
        stack = None
        if 3 in streams:
            _, trva = streams[3]
            for i in range(u32(b, trva)):
                o = trva + 4 + 48 * i
                if u32(b, o) == tid:
                    stack = (u64(b, o + 24), u32(b, o + 32), u32(b, o + 36))
        print("  rip=0x%x rsp=0x%x" % (rip, rsp))
        if stack:
            start, dsize, srva = stack
            print("  stack: 0x%x..0x%x (%d bytes)" % (start, start + dsize, dsize))
            seen, depth = [], 0
            off = max(0, rsp - start) if start <= rsp < start + dsize else 0
            for p in range(off, min(dsize, off + 0x4000) - 7, 8):
                v = u64(b, srva + p)
                for base, size, name in mods:
                    if base <= v < base + size and v not in seen:
                        seen.append(v)
                        print("   +0x%04x  %s+0x%x" % (p, name.rsplit("\\", 1)[-1], v - base))
                        depth += 1
                        break
                if depth >= 40:
                    break
    print("modules (%d); exe + non-system:" % len(mods))
    for base, size, name in mods:
        low = name.lower()
        if "windows" in low and "system32" in low:
            continue
        print("  0x%x %8x %s" % (base, size, name))


if __name__ == "__main__":
    main(sys.argv[1])
