# PROB.g1a — how the official add-in reaches the OS (and where the USB dialog comes from)

Scope: static analysis of `E:\fxtxtviewer\PROB.g1a` plus the three control add-ins.
All scripts live next to this file (`p01`…`p13`, shared `sh4lib.py`).
Nothing outside `E:\fxtxtviewer\work\probe\` was modified; nothing was built or flashed.

---

## 1. Byte order and code range

**Big-endian SH-4A.** Evidence:

* `0x00300000  d30c  mov.l 0x300034,r3` … `432b jmp @r3` decodes coherently only as BE;
  a little-endian decode of the same bytes yields no valid instruction.
* All 98 occurrences of the 4-byte pattern `80 01 00 70` (the syscall gateway
  `0x80010070`) are big-endian; the LE count is 0. `0x80020070` (fx-CG50 gateway)
  occurs 0 times.
* Capstone needs `CS_MODE_BIG_ENDIAN | CS_MODE_SH4A`; without `SH4A` it falls back to
  SH-1 and rejects `bt/s`/`bf/s` (`8d0c`, `8f41`), which is why an earlier pass saw
  "26 % undecodable halfwords".

Code range analysed: file `0x200 … 0xf044` → VA `0x00300000 … 0x0030ee44`
(61508-byte file, sha256 `1afab3f95a8df20a6590bffa43fc13eec06abfb2409dd4945839ef0aefab88cc`).

Container: 32-byte G1A header + 480-byte add-in header ⇒ payload at file `0x200`.
That mapping is *verified*, not assumed: the 98 syscall stubs are found as data at
file `0x7b20`, and all 98 stub addresses are then referenced **with exact equality**
from literal pools inside the code. Any other file→VA offset would make those 98
matches fail.

Add-in identity (file `0x20`): internal name `@PROBSIM`, version `01.01.0000`,
date `2019.0807.1123`, display name `Prob Sim`. It is an ordinary add-in — there is
no entry-address field in the header (scanning `0x000–0x1ff` for `0x300000–0x310000`,
`0x8000xxxx`, `0xA4xxxxxx` finds only `80000008` at `0x0b8`/`0x0d8`), so the OS
enters at `base + 0` = `0x00300000`.

---

## 2. The calling convention (proven)

Every OS call goes through a 16-byte stub. Verbatim bytes of stub #81 at `0x00307e30`:

```
00307e30  d201       mov.l @(4,pc),r2      ; -> 0x00307e38
00307e32  d002       mov.l @(8,pc),r0      ; -> 0x00307e3c
00307e34  422b       jmp   @r2             ; -> 0x80010070
00307e36  0009       nop
00307e38  80010070   .long 0x80010070      ; OS syscall gateway
00307e3c  00000c4f   .long 0x00000c4f      ; syscall id
```

So:

* **syscall id in `r0`**, arguments in `r4, r5, r6, r7` (normal SH-4 ABI),
  result in `r0`, called with `jsr` (the stub tail-jumps, so `pr` must be valid).
* The gateway itself is `0x80010070`; you do not need the stub — `mov #id,r0` +
  `mov.l @(gateway_literal),rN` + `jsr @rN` is equivalent.
* This is exactly gint's `syscall_(id, 0x80010070)` macro shape and the shape of
  Casio's own `fxlib` imports, so `0x80010070` is the fx-9860G/fx-9750GIII entry
  into the OS syscall dispatcher.

There is **no other route into the OS**: 1870 direct `bsr/bra` in the payload, zero of
which land on a stub; no PLT table; the only OS-range literals in the whole payload are
`0x80010070` (×98), `0x80000000` (×2, at `0x30755e`, `0x307658`) and `0x8801e000`
(×2, RAM).

---

## 3. Inventory: 98 imports, 66 actually called

98 contiguous stubs, `0x00307920 … 0x00307f40`, 16 bytes each (id = the dword after the
gateway). The linker pulled the whole fxlib object, so table membership ≠ usage.

Called ids, with call-site counts (full site lists via `p06_imports.py`):

| id | n | ids | n | ids | n |
|---|---|---|---|---|---|
| 0x0ad6 | 7 | 0x0c4f | **6** | 0x0914 | 4 |
| 0x05c0 | 6 | 0x0840 | 6 | 0x05c7 | 4 |
| 0x060a | 6 | 0x0832 | 6 | 0x05e8,0x0808,0x0eac,0x0809,0x05e7,0x0936,0x0eab | 3 |
| 0x0acf | 6 | 0x0ad0 | 6 | 0x05a4,0x091b,0x0829,0x09ad,0x0954,0x0834,0x04b0,0x0c12,0x0783,0x04c6,0x08da,0x0784,0x082b,0x05a6,0x05e6 | 2 |
| 0x05c8 | 6 | | | 0x04c4,0x029a,0x08fe,0x080f,0x0462,0x0807,0x090f,0x05bf,0x0818,0x04e6,0x05c9,0x04e9,0x0013,0x0844,0x0810,0x0218,0x03fa,0x0841,0x0814,0x04cc,0x08d8,0x04b9,0x08dc,0x0813,0x04cd,0x0544,0x04b8,0x0545,0x04cb,0x0913,0x04bc,0x0916,0x05a7 | 1 |

The "×6" groups all appear in six near-identical functions (sites ×6 at
`0x3001c8/0x302324/0x302652/0x3035fc/0x305b10/0x306e4e` etc.) — PROB has six screens
built from one template.

Names confirmed from gint's fx-9860G table (external, `src/kernel/syscalls.S`):
`0x0acd malloc, 0x0acc free, 0x042c-0x043d BFile_*, 0x0118-0x011b Timer_*, 0x0241
ClearKeyBuffer, 0x0247 GetKeyWait, 0x0248 PutKeyCode, 0x0135 GetVRAMAddress,
0x0494 SetQuitHandler, 0x0236 Reset`.

**What PROB does *not* use matters as much as what it does:**

* none of the BFile ids (`0x042c–0x043d`) are called — no file I/O;
* `GetKeyWait 0x0247`, `ClearKeyBuffer 0x0241`, `PutKeyCode 0x0248` are in the table but
  **never called**;
* `SetQuitHandler 0x0494` is in the table but **never called**;
* `Timer_Install/Start/Stop/Deinstall` (0x0118–0x011b) are in the table but never called
  ⇒ **PROB installs no interrupt handler and no timer**;
* `GetVRAMAddress 0x0135` and `Reset 0x0236` are not even in the table;
* `0x0AAE` — the community-known "switch to RUN.MAT / app mode" id — is **not in the
  table at all**, so PROB does not raise the dialog by switching to the LINK app;
* `0x0eab` / `0x0eac` are called 3× each (`0x3002dc`, `0x301342`, `0x303992` /
  `0x3059c6`, `0x306a9a`, `0x306b62`), never in the idle loop. Noted only because on the
  fx-CG `GetKey` is syscall `0x0EAB`; the fx-9750GIII uses the fx-9860G id space (its
  heap ids cluster around `0x0acd malloc`), so that coincidence must not be used directly.

---

## 4. The per-frame loop — where the OS gets control

Main function `0x003001c4` (prologue at `0x3001c4–0x3001e2`, `rts` at `0x30029e`);
its idle loop:

```
00300224  d227       mov.l 0x3002c4,r2      ; *(0x3002c4) = 0x00308060
00300226  420b       jsr   @r2             ; display/refresh helper (no args, result ignored)
00300228  0009       nop
0030022a  d327       mov.l 0x3002c8,r3      ; *(0x3002c8) = 0x00307e30  = stub #81
0030022c  430b       jsr   @r3             ; SYSCALL 0x0c4f
0030022e  64f3       mov   r15,r4          ;   delay slot: r4 = &keycode (stack buf)
00300230  2008       tst   r0,r0
00300232  8bf7       bf    0x300224        ; loop while the call returns non-zero
00300234  60f2       mov.l @r15,r0         ; key code
00300236  882f       cmp/eq #47,r0         ; 0x2F
00300238  8d0c       bt/s  0x300254
0030023a  6ed3       mov   r13,r14
0030023c  8830       cmp/eq #48,r0         ; … through #52 (0x30..0x34), r14 = mode index
0030023e  890b       bt    0x300258
...
0030026a  63ef       exts.w r14,r3
0030026c  4311       cmp/pz r3
0030026e  8bd9       bf    0x300224        ; -1 (no match) -> poll again
00300270  d316       mov.l 0x3002cc,r3
00300276  430b       jsr   @r3            ; -> 0x003012aa(main_arg, r14, 3)
```

Signature inferred: `int f(unsigned short *keycode)` — one out-pointer in `r4`,
`r0` = status (0 = nothing, non-zero = key available), key code written to `*r4`.
That is the shape of an fxlib `GetKey`-style poll, **not** of
`GetKeyWait(sel, time, menu, &keycode)` (0x0247), which PROB never calls.

Other five call sites of `0x0c4f` confirm the same signature:

```
00300314  d3..  mov.l …,r3          ; drain helper: do { r = f(arg); } while (r != 0);
0030031c  430b  jsr @r3
...
003022e6  64f3  mov   r15,r4
003022ea  4b0b  jsr   @r11           ; f(r15+44)
003022f0  8801  cmp/eq #1,r0
003022f2  8bf9  bf    0x3022e6       ; loops until it returns exactly 1  ("wait for a key")
...
00303740  430b  jsr   @r3            ; f(r15+12); tst r0,r0 / bt/s; result saved @(24,r15)
003026c8, 00306f4c                 ; same pattern in the other two screen templates
```

`0x0c4f` is therefore **the only OS entry point that PROB executes continuously**.
Everything else is init-time or per-draw.

The other per-frame call, `0x00308060`, is always invoked with no arguments and its
`r0` is ignored (`jsr @r2; nop;` followed by unrelated code) ⇒ a `void f(void)`
frame-refresh helper. Caveat: that address decodes as the third of eight
`3104 div1 r0,r1` inside a libgcc-style division routine at `0x0030804c` — see §7.

---

## 5. PROB never touches the USB hardware

Automated scans (`p08_ptrs.py`) over every literal-pool pointer:

* pointers into the peripheral window `0xA4xxxxxx`: **0** — no USB `0xA4D80000`,
  no INTC `0xA4D00000`, no CPG `0xA4150000`;
* pointers into `0x88xxxxxx`: only `0x8801e000` (×2);
* OS-range pointers: `0x80010070` (×98), `0x80000000` (×2);
* no string containing `usb`, `conn`, `cable`, `link`, `serial`, `mode`
  (checked ASCII and UTF-16BE).

⇒ PROB cannot poll the cable itself and installs no USB interrupt handler.
Whatever it does about a plugged cable, the OS does it **inside one of the 66
syscalls above**, and the only one running all the time is `0x0c4f`.

---

## 6. Control add-ins

| file | gateway occurrences | syscall stubs | notes |
|---|---|---|---|
| `PROB.g1a` | 98 | 98 contiguous `0x307920…0x307f40` | fxlib, 66 called |
| `EREADER.G1A` | 39 | 39, non-contiguous | fxlib; BFile family present (`0x042c–0x0439`) |
| `DL22P.G1A` | 42 | 42, non-contiguous | fxlib; BFile family present |
| `fxTap.g1a` | **0** | 0 | **not fxlib at all — gint-based** |

* `fxTap.g1a` (360932 bytes) contains no `0x80010070` whatsoever and no stubs: it is a
  gint add-in. Its non-detection is *structural* — gint takes over interrupts and the
  OS never runs while the add-in does. Nothing about "which syscall" can explain it;
  it simply never calls the OS.
* `EREADER`/`DL22P` are fxlib add-ins but their stub addresses are **not** referenced
  from literal pools and are **not** direct branch targets: they call the OS through
  computed pointers (`mov.l @(r0,rM),rN` off an import table). 52 (EREADER) and 47
  (DL22P) such indirect calls remain unresolved, so a verified "used-id" set for the
  controls is *not* available. What is clear: their tables also contain `0x0c4f`,
  `0x0244`, `0x0494`, `0x0462`, `0x0807`, `0x082b`, `0x0840`, `0x0841`, `0x0844` and the
  whole BFile family — i.e. **there is no "USB-only" id in any of the four tables**;
  the difference between the apps is behavioural, not which imports exist.

---

## 7. Most likely mechanism, and how to use it

**The dialog is raised inside syscall `0x0c4f`** (the OS key/event poll), which is the
only OS code that runs while PROB idles:

```asm
0030022a  d327       mov.l 0x3002c8,r3   ; -> 0x00307e30 (stub #81)
0030022c  430b       jsr   @r3           ; r0 = 0x0c4f through stub
0030022e  64f3       mov   r15,r4        ; &keycode
00300230  2008       tst   r0,r0
00300232  8bf7       bf    0x300224     ; keep polling
```

Implementable equivalent (no stub needed), once per frame / idle iteration:

```c
/* a = &u16 keycode;  r0 = syscall id;  jsr to the gateway */
register unsigned id  asm("r0") = 0x0c4f;
register void    *arg asm("r4") = &keycode;
asm volatile("mov.l 1f, r2\n\tjsr @r2\n\tnop\n\tbra 2f\n\tnop\n"
             ".align 2\n1:\t.long 0x80010070\n2:" : "+r"(id) : "r"(arg) : "r1","r2","pr");
/* returns: 0 = nothing pending, non-zero = keycode valid in keycode */
```

The community-standard way to write this without a per-id stub (martin.poupe / cncalc idiom,
5th argument = syscall id on the stack):

```c
static const int SysCallWrapper[] = { 0xD201422B, 0x60F20000, 0x80010070 };
/*  0xD201422B = mov.l @(4,pc),r2 ; jmp @r2      (jump to the gateway)
    0x60F20000 = mov.l @(0,r15),r0                (id = 5th C argument)
    0x80010070 = the gateway itself                             */
static const int (*syscall5)(int r4, int r5, int r6, int r7, int id)
    = (void *)&SysCallWrapper;

syscall5((int)&keycode, 0, 0, 0, 0x0c4f);   /* OS key/event poll, once per frame */
```

Practical notes for the add-in being written:

* keep running with the OS's interrupts enabled (do not take over the exception
  vectors as gint does) — the OS USB interrupt must still be delivered;
* poll `0x0c4f` every frame from the idle loop (`while (f(&k)) ;` is what PROB does);
* expect keycodes `0x2f, 0x30…0x34` in the normal menu path, but do not rely on that —
  just discard unknown codes and keep polling.

Confidence: **medium-high** that the OS's key/event syscall is the vector (it is the
only OS code that runs; PROB has no other channel); **medium** that the exact id is
`0x0c4f`, because no authoritative fx-9860G id→name table was obtained. If `0x0c4f`
does not do it, the next candidate is `0x0247 GetKeyWait` (`sel, time, menu, &keycode`);
it is the SDK's documented "wait for a key, with OS handling" call, but note that PROB
deliberately does *not* use it and still shows the dialog.

---

## 8. Caveat that must be resolved before trusting PROB's call graph: the file looks modified

Three of PROB's own payload pointers cannot be executed:

1. entry `0x00300000` = `d30c mov.l 0x300034,r3; 7ffc add #-4,r15; …; 432b jmp @r3`
   with `*(0x300034) = 0x00307aaa`. Mapping-independent argument: stub bases are
   16-byte aligned and `0x307aaa ≡ 0xa (mod 16)`, so *whatever* the load base is, this
   value addresses the **middle of a stub's gateway literal** (`8001 0070`), not code.
   The same code is reachable at `0x00300010`: `d309 mov.l 0x300038,r3; 430b jsr @r3`
   with `*(0x300038) = 0x00300244`, which is `8832 cmp/eq #50,r0` — the middle of the
   function that starts at `0x003001c4`. So neither entry path executes.
2. `*(0x003002c4) = 0x00308060` = 3rd of eight `div1 r0,r1` in the division routine at
   `0x0030804c` (not a function start).
3. the format-string argument `0x00308508` passed to `0x0030131c` points into
   font/graphics data (`3f f0 3f e0 7f e0 …`), not a text string.

By contrast the two control add-ins start with the canonical 3-instruction crt0
(`EREADER`: `d301 mov.l 0x300008,r3; 432b jmp @r3` → `0x00300e36`; `DL22P`:
`d601 … 462b jmp @r6` → `0x00301d1e`), and the G1A container magic is identical in all
four files. So the anomaly is specific to PROB.g1a.

Consequence: the *structure* reported above (stub shape, ids, 0x0c4f call pattern) is
read directly from bytes and is reliable; but the call-graph detail "which function is
main" and the identity of `0x308060` should be re-checked against a pristine
`PROB.g1a`. Please verify the sha256 above against the official release, or hand me an
unmodified copy.

---

## 9. What is missing / what else could be tried

1. **Pristine PROB.g1a** (see §8) — cheapest way to remove the last doubt.
2. **An authoritative fx-9860G id→name table.** gint only wraps ~25 ids; the community
   document (`https://community.casiocalc.org/topic/3962-fx-9860g-system-call-and-protocol-documentation/`)
   claims ~4000 dispatcher entries. With it, `0x0c4f`, `0x0832`, `0x05c0`, `0x05c8`,
   `0x0ad0`, `0x0840`, `0x0acf`, `0x060a`, `0x0eab/0x0eac`, `0x0c12` get names.
3. **Empirical 20-line test** (decisive, cheap): build one add-in that only loops
   `0x0c4f(&k)` and one that only loops `0x0247`, plug the cable into each.
4. **OS-image scan**: dump the fx-9750GIII OS, locate the syscall dispatcher table
   (index 0x0c4f) and inspect its handler for references to the connection-mode /
   "USB Flash" screen — that would name the mechanism outright.

---

## 10. External corroboration (fetched during this analysis)

* **[SimLo, "fxReverse2x"](https://bible.planet-casio.com/simlo/chm/fxReverse2x.pdf)** —
  independent confirmation of §2: the OS keeps a table of pointers to all API functions,
  "the code to access it lies at ROM address `0x80010070`", the table has thousands of
  entries (version-dependent, e.g. 1.04 adds `0x1032 Bkey.GetKeyTableInfoJumpFunc`), and
  "the functions use the Renesas calling convention, first four arguments in registers
  r4 to r7, and additional arguments on the stack", with exactly the
  `mov.l syscall_number,r0 / mov.l syscall,r2 / jmp @r2 / nop / syscall: .long 0x80010070`
  sequence found 98× in PROB.
* **Official fx-9860G SDK, "Libraries" manual**
  ([Libraries.pdf](https://bible.planet-casio.com/common/casio/sdk_manuals/Libraries.pdf)) —
  says of `Bdisp_PutDisp_DD`: "transfers all bitmap data from VRAM to DD (Display Driver)…
  **The Bdisp_PutDisp_DD function is called in the GetKey function too.**"
  ⇒ the OS's `GetKey` is a fat housekeeping routine (display flush *and* key scan), which
  is exactly where an OS-side cable check belongs. This is the strongest support for
  §7: the imported key call is the OS's own periodic service entry point.
* **[Omnimaga, "Casio Prizm documentation" (p.460)](https://www.omnimaga.org/casio-calculator-programming-news-and-support/casio-prizm-documentation/460/?wap2)** —
  "GetKey, the blocking keyboard read function: interface `int GetKey(int *keycode)` …
  on the fx-CG the underlying syscall is `0x0EAB`". Same *shape* as the `0x0c4f` call
  reconstructed in §4 (one out-pointer, status in `r0`); fx-CG ids differ from
  fx-9860G/fx-9750GIII ids, so `0x0EAB` is **not** the id to use here.
* **[cnCalc "syscall usage" tutorial](https://www.cncalc.org/archiver/tid-7744.html)** —
  the `SysCallWrapper` idiom quoted in §7, and proof that OS *UI/mode* functions are
  reachable this way (`App_RUN_MAT()` = syscall `0xAAE` switches to RUN.MAT). Note PROB
  never calls `0xAAE`/mode-switch ids, i.e. **it does not raise the dialog by switching
  to the LINK app**.
* **Casio fx-9860GII/fx-9750GIII user guide** ("Select Connection Mode Screen", all models
  except fx-7400GII): "Connecting the USB cable to the calculator will cause the dialog
  box shown nearby to appear" — the dialog belongs to the OS, not to the add-in.

