"""Enumerate every call into the OS for one or more G1A add-ins.

Usage:  python p04_calls.py [--detail id[,id...]] file.g1a [...]

Finds (1) the fxlib syscall stub block, (2) every indirect call/jump that
resolves to the 0x80010070 syscall gateway (inline trampoline) or to a stub in
the block, and prints per-syscall call counts plus the call-site addresses.
"""
import sys
import sh4lib

NAMES = {
    # fx-9860G ids confirmed by gint src/kernel/syscalls.S
    0x0acd: "malloc",
    0x0acc: "free",
    0x0e6d: "realloc",
    0x042c: "BFile_Open",
    0x042d: "BFile_Close",
    0x042f: "BFile_Size",
    0x0431: "BFile_Seek",
    0x0432: "BFile_Read",
    0x0434: "BFile_Create",
    0x0435: "BFile_Write",
    0x0439: "BFile_Remove",
    0x043b: "BFile_FindFirst",
    0x043c: "BFile_FindNext",
    0x043d: "BFile_FindClose",
    0x0118: "Timer_Install",
    0x0119: "Timer_Deinstall",
    0x011a: "Timer_Start",
    0x011b: "Timer_Stop",
    0x0135: "GetVRAMAddress",
    0x0241: "ClearKeyBuffer",
    0x0247: "GetKeyWait",
    0x0248: "PutKeyCode",
    0x0494: "SetQuitHandler",
    0x0236: "Reset",
}


def analyse(path, detail_ids):
    img = sh4lib.Image(path)
    insns = img.insns()
    order = sorted(insns)
    stubs = img.stubs()
    stub_by_va = dict(stubs)

    print("=" * 78)
    print("%s  size=%d code=%08x..%08x  insns=%d  stubs=%d"
          % (path, len(img.data), sh4lib.BASE, img.end, len(insns), len(stubs)))

    # the stubs' own `jmp @r2` also resolves to the gateway; those are not
    # call sites, they ARE the stubs.  Block = first stub .. first stub + n*16.
    lo = min(stubs)
    hi = lo + 16 * len(stubs)

    calls = {}      # id -> [call site]
    ptrs = {}       # other pointer -> [call site]
    unresolved = []  # (site, reg)
    for idx, a in enumerate(order):
        i = insns[a]
        if i.m not in ("jsr", "jmp") or not i.o.startswith("@"):
            continue
        reg = i.o[1:].strip()
        if not reg.startswith("r"):
            continue
        if lo <= a < hi:
            continue
        kind, val, via = sh4lib.resolve_indirect(insns, order, idx, reg, img)
        if kind == "syscall":
            calls.setdefault(val, []).append(a)
        elif kind == "ptr":
            if val in stub_by_va:
                calls.setdefault(stub_by_va[val], []).append(a)
            else:
                ptrs.setdefault(val, []).append(a)
        else:
            unresolved.append((a, reg))

    print("  resolved to a syscall: %d sites over %d ids; other ptr targets: %d; unresolved: %d"
          % (sum(len(v) for v in calls.values()), len(calls),
             len(ptrs), len(unresolved)))
    print("  stub table: %s" % " ".join("%03x" % stubs[k] for k in sorted(stubs)))
    print()
    print("  %-6s %-5s %s" % ("id", "calls", "name / call sites"))
    for sid in sorted(calls, key=lambda x: -len(calls[x]) if x is not None else 0):
        sites = calls[sid]
        nm = NAMES.get(sid, "")
        print("  %-6s %-5d %-22s %s"
              % ("0x%04x" % sid if sid is not None else "??",
                 len(sites), nm, " ".join("%08x" % s for s in sites[:12])))
    if ptrs:
        print("\n  other indirect targets (function pointers, not syscalls):")
        for v in sorted(ptrs):
            print("    0x%08x  x%-3d  %s" % (v, len(ptrs[v]),
                                             " ".join("%08x" % s for s in ptrs[v][:8])))
    if detail_ids:
        for sid in detail_ids:
            sites = calls.get(sid, [])
            if not sites:
                print("\n  --- id 0x%04x: NO calls" % sid)
                continue
            print("\n  --- context for id 0x%04x (%s), %d sites"
                  % (sid, NAMES.get(sid, "?"), len(sites)))
            for s in sites[:8]:
                idx = order.index(s)
                lo = max(0, idx - 12)
                print("    @ %08x" % s)
                for k in range(lo, min(len(order), idx + 3)):
                    i = insns[order[k]]
                    print("      %s %s %s %s"
                          % ("->" if i.a == s else "  ", "%08x" % i.a, i.m, i.o))
    return calls, stubs, ptrs


if __name__ == "__main__":
    args = sys.argv[1:]
    detail = []
    if args and args[0] == "--detail":
        detail = [int(x, 16) for x in args[1].split(",") if x]
        args = args[2:]
    for p in args:
        analyse(p, detail)
