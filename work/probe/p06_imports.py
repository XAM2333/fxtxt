"""Complete import call-site finder for a Casio G1A.

Method (robust, no register-lineage guessing):
  1. index every 4-byte-aligned dword in the payload
  2. for each stub address S, find literal slots L whose dword == S
  3. find every instruction that reads L as a pc-relative literal
  4. from there walk forward to the jsr/jmp @rN that uses that register

Usage: p06_imports.py file.g1a [--detail id,id] [--args]
"""
import sys
import sh4lib

NAMES = {
    0x0acd: "malloc", 0x0acc: "free", 0x0e6d: "realloc",
    0x042c: "BFile_Open", 0x042d: "BFile_Close", 0x042f: "BFile_Size",
    0x0431: "BFile_Seek", 0x0432: "BFile_Read", 0x0434: "BFile_Create",
    0x0435: "BFile_Write", 0x0439: "BFile_Remove", 0x043b: "BFile_FindFirst",
    0x043c: "BFile_FindNext", 0x043d: "BFile_FindClose",
    0x0118: "Timer_Install", 0x0119: "Timer_Deinstall", 0x011a: "Timer_Start",
    0x011b: "Timer_Stop", 0x0135: "GetVRAMAddress", 0x0241: "ClearKeyBuffer",
    0x0247: "GetKeyWait", 0x0248: "PutKeyCode", 0x0494: "SetQuitHandler",
    0x0236: "Reset",
}


def load(path):
    img = sh4lib.Image(path)
    insns = img.insns()
    stubs = img.stubs()
    return img, insns, stubs


def refs_to(img, insns, targets):
    """targets: set of VAs.  Return {target: [(insn_addr, reg, lit_va)]}."""
    order = sorted(insns)
    # 1+2: literal slots holding a target address
    slots = {}
    for a in range(sh4lib.BASE, img.end - 2, 4):
        v = img.u32(a)
        if v in targets:
            slots.setdefault(a, v)
    # 3: instructions reading those slots
    out = {t: [] for t in targets}
    for a in order:
        i = insns[a]
        if i.m not in ("mov.l", "mova"):
            continue
        head = i.o.split(",")[0].strip()
        if not head.startswith("0x"):
            continue
        try:
            lit = int(head, 16)
        except ValueError:
            continue
        if lit not in slots:
            continue
        tgt = slots[lit]
        rest = i.o.split(",")[1].strip() if "," in i.o else ""
        reg = rest if rest.startswith("r") else "r0"   # mova -> r0
        # 4: walk forward to the branch using that register
        site = None
        for k in range(order.index(a) + 1, min(len(order), order.index(a) + 12)):
            j = insns[order[k]]
            if j.m in ("jsr", "jmp") and j.o.strip() == "@" + reg:
                site = j.a
                break
            # register clobbered? stop (simple heuristic: any write to reg)
            if j.o and [p.strip() for p in j.o.split(",")][-1] == reg:
                break
        out[tgt].append((site if site else a, reg, lit, a))
    return out


def arg_setup(insns, order, site, maxback=14):
    """Collect the immediate/pointer setup of r4..r7 before a call site."""
    idx = order.index(site)
    got = {}
    for k in range(idx - 1, max(0, idx - maxback), -1):
        i = insns[order[k]]
        if i.m in ("jsr", "jmp", "rts", "bra"):
            break
        if not i.o or not i.o.split(",")[-1].strip().startswith("r"):
            continue
        dst = i.o.split(",")[-1].strip()
        if dst in got or dst not in ("r4", "r5", "r6", "r7"):
            continue
        if i.m in ("mov", "mov.w", "mov.l", "mova"):
            got[dst] = "%s %s" % (i.m, i.o)
    return got


def main():
    args = sys.argv[1:]
    detail = []
    show_args = False
    if args and args[0] == "--detail":
        detail = [int(x, 16) for x in args[1].split(",") if x]
        args = args[2:]
    if args and args[0] == "--args":
        show_args = True
        args = args[1:]
    path = args[0]

    img, insns, stubs = load(path)
    order = sorted(insns)
    lo = min(stubs) if stubs else 0
    hi = lo + 16 * len(stubs) if stubs else 0
    print("=" * 78)
    print("%s  stubs=%d (%08x..%08x)  insns=%d" % (path, len(stubs), lo, hi, len(insns)))

    if not stubs:
        print("  NO fxlib syscall stubs at all -> not an fxlib add-in")
        return

    r = refs_to(img, insns, set(stubs))
    by_id = {}
    for s, lst in r.items():
        sid = stubs[s]
        for (site, reg, lit, at) in lst:
            by_id.setdefault(sid, []).append((site, reg, lit, s))
    print("  %d stub addresses referenced, %d distinct syscalls called"
          % (len(stubs), len(by_id)))
    print()
    for sid in sorted(by_id, key=lambda x: -len(by_id[x])):
        sites = sorted(set(x[0] for x in by_id[sid]))
        print("  0x%04x %-18s x%-3d %s"
              % (sid, NAMES.get(sid, ""), len(sites),
                 " ".join("%08x" % s for s in sites[:10])))
    print()
    print("  stubs never referenced: %s"
          % " ".join("%03x" % stubs[s] for s in sorted(stubs) if stubs[s] not in by_id))
    print("  CALLED IDS (sorted): %s"
          % " ".join("%04x" % s for s in sorted(by_id)))

    if detail:
        for sid in detail:
            print("\n" + "-" * 70)
            print("id 0x%04x (%s)" % (sid, NAMES.get(sid, "?")))
            for s in sorted(stubs):
                if stubs[s] != sid:
                    continue
                print("  stub @ %08x" % s)
                for (site, reg, lit, at) in r[s]:
                    print("    literal 0x%08x read by insn %08x -> call %08x via %s"
                          % (lit, at, site, reg))
                    if show_args:
                        for kk in sorted(arg_setup(insns, order, site).items()):
                            print("        %s = %s" % kk)


main()
