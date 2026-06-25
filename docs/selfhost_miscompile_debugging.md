# Debugging self-host miscompiles (armv8m-tcc)

A **self-host miscompile** is when the **cross** compiler (`bin/armv8m-tcc`, an x86
binary built by gcc that *emits* ARM Thumb-2) compiles tinycc's own source into a
**native** compiler (the ARM `armv8m-tcc` that runs on the device) whose machine
code is subtly wrong. The source is correct — the same tinycc logic compiles a
test correctly when run as the cross, but wrong when run as the self-hosted
native binary. Symptom: a test program built **on the device** misbehaves
(infinite loop, wrong output, HardFault) even though the host cross builds it
fine.

Most remaining `tests2` failures are this class. This guide is the repeatable
workflow to nail them. Worked example throughout: `09_do_while` (do-while loop
ran forever — fixed in `ir/regalloc.c ra_resolve_phis`).

---

## 0. The mental model (read this first)

```
gcc ──compiles──> bin/armv8m-tcc        (CROSS: x86 host binary, emits ARM)
                       │
                       │ compiles tinycc's own *.c  ← a bug HERE is the culprit
                       ▼
                  native armv8m-tcc      (rootfs/usr/bin/tcc: ARM, runs on device)
                       │
                       │ compiles tests2/NN.c
                       ▼
                  /tmp/NN  (device binary that misbehaves)
```

Two independent facts pin it as a self-host bug:
1. **Host cross compiles the test correctly** — so the test source and tinycc
   *logic* are fine.
2. **Device (native) compiles it wrong** — so the native binary's code for some
   tinycc function `F` is wrong, i.e. **the cross miscompiled `F`**.

There are two fix strategies (both valid, §6):
- **(A) Source workaround** in the tinycc function `F`: rewrite `F` so the cross
  happens to compile it correctly. Fast, local, low-risk. (What `09` used.)
- **(B) Fix the cross codegen bug** itself: find the wrong ARM the cross emits and
  fix the cross's optimizer/backend. Harder, but fixes *every* test that trips the
  same bug at once. Prefer this when the same bug class recurs.

---

## 1. Fast device round-trips: the FAT drive (use this, not RAM-scan)

The slow/flaky way (`scripts/qemu_capture_yaff.py`) scans guest RAM for binaries.
The fast way is the host-readable FAT drive mounted at **`/mnt`** on the QEMU
guest — drop sources in, pull device-compiled binaries out, **no kernel rebuild**.
See [memory: yasos-qemu-fatdisk-host-drive] for the full design. One-liner:

```bash
.qemu_smoke_venv/bin/python3 scripts/qemu_fatdisk_run.py \
  --put libs/tinycc/tests/tests2/09_do_while.c:IN.C \
  --cmd 'tcc -x c /mnt/IN.C -o /mnt/OUT; echo CC=$?; /mnt/OUT; echo RC=$?' \
  --get OUT:.cache/09_dev.elf \
  --backing .cache/bk.bin --img .cache/fd.img --boot-wait 7 --timeout 14
```

- `--put HOST:FATNAME` puts a file on the drive; `--get FATNAME:HOST` pulls one out.
- `--cmd` runs on the guest shell; stdout/stderr stream live to the log (a runaway
  guest is bounded by `--timeout`, not infinite).
- **8.3 UPPERCASE names only** (FatFs `FF_USE_LFN=0`): a source lands as `IN.C`;
  tcc rejects `.C` → **always pass `tcc -x c`**.
- **Don't `ls /mnt`** — a kernel FatFs readdir bug panics ("invalid enum value").
  Compiling (open/read/write) is fine.
- It needs the QEMU kernel built with the `/mnt` drive support (already in tree:
  `hal/.../ramflash.zig`, `linker_script.ld` fatdisk window, `main.zig` mount).

Carve + disassemble the captured YAFF binary (`main` is after the crt0 stub —
look for `push {r4,...}` / `movs r4,#1`):

```bash
python3 - <<'PY'
import struct; d=open('.cache/09_dev.elf','rb').read()
cl=struct.unpack_from('<I',d,8)[0]; off=struct.unpack_from('<H',d,70)[0]
open('.cache/09_dev.text','wb').write(d[off:off+cl])
PY
arm-none-eabi-objdump -D -b binary -m arm -M force-thumb .cache/09_dev.text
```

---

## 2. Confirm it's a self-host bug (host vs device)

Compile the test with the **host cross** to an ELF and disassemble the same
function; if the host is correct and the device is wrong, it's self-host:

```bash
cd libs/tinycc
./bin/armv8m-tcc tests/tests2/09_do_while.c -o /tmp/host.elf -Wl,-oformat=elf32-littlearm
arm-none-eabi-objdump -d -Mforce-thumb /tmp/host.elf   # find main; compare to device
```

For `09`: host `main` ended the loop with `bge.w 0xf4` (epilogue); device emitted
`bge.w 0xee` (mid-body) → never exits. Same structure, one wrong branch target →
self-host.

Also useful: `-O0` does **not** reliably isolate it — building the *native* tcc at
`-O0` shifts the bug to a *different* self-host miscompile (e.g. the `<command line>`
macro bug) and often won't even compile. Don't trust `-O0`-native as a bisector.

---

## 3. Localize the miscompiled tinycc function

This is the heart of the work. Narrow from "the test is wrong" to "tinycc
function `F`, this exact computation".

### 3a. Narrow the *language feature* (cheap, FAT-drive)
Build one test program exercising several constructs and see which misbehaves.
`09` narrowed to **do-while only** (a `for`+`while`+`do-while` program: `for`/`while`
exited, `do-while` ran forever) → the bug is on the do-while codegen path.

### 3b. See the IR and which *pass* transforms it (host, instant)
Build a **debug cross** (dumps IR; no device needed). Clean stale objects first —
a prior native build leaves ARM `.o`s that break the x86 cross link
("file in wrong format"):

```bash
cd libs/tinycc
rm -rf armv8m-arch armv8m-ir armv8m-*.o *.o arm-eabi-*.o
SR=$PWD/../../rootfs
./configure --extra-cflags="-DTCC_DEBUG=1 -DCONFIG_TCC_DEBUG=1 -g -O1 -DTARGETOS_YasOS=1 -DCONFIG_TCC_BCHECK=0" \
  --enable-cross --config-asm=yes --config-pie=yes --config-pic=yes --debug --enable-O1 \
  --prefix=$PWD --sysroot=$SR --sysincludepaths="{B}/include:$SR/usr/include" \
  --crtprefix="$SR/usr/lib" --libpaths="$SR/usr/lib:$SR/lib"
make armv8m-tcc -j8
./armv8m-tcc -dump-ir            -c tests/tests2/09_do_while.c -o /tmp/x.o   # 3 checkpoints
./armv8m-tcc -dump-ir-passes=all -c tests/tests2/09_do_while.c -o /tmp/x.o   # after every pass
```

Diff the IR across passes to find the one that produces the wrong shape. For `09`
the inverted exit branch only appears in the **"AFTER OPTIMIZATIONS"** dump using
`R`-registers → it's introduced during **register allocation** (after the last
`-dump-ir-passes` checkpoint), specifically the phi-copy insertion in
`ra_resolve_phis`. (NB this debug cross is correct — it shows the *intended* IR,
e.g. exit target = 18. The device computes a different value; the gap localizes it.)

### 3c. Get the *device's* actual values (one native rebuild)
When the IR transform is the suspect, add a one-off `fprintf(stderr, ...)` to the
relevant pass dumping the indices/targets it computes, rebuild the native tcc,
and run on the device via the FAT drive. For `09`, instrumenting
`tcc_ir_codegen_backpatch_jumps` printed `target_ir=15` (should be 18) for the
exit JUMPIF — proving the **target index in the IR was already wrong**, not the
address encoding. Remove the instrumentation afterwards.

Rebuild native + kernel (the device tcc lives in the incbin'd romfs):
```bash
rm -f libs/tinycc/.yasos-build/native-stage1.stamp libs/tinycc/.yasos-build/native-stage2.stamp
./build_rootfs.sh -o rootfs.img        # cross unchanged → only native rebuilds (~3-5 min)
rm -rf .zig-cache && zig build -Doptimize=ReleaseSafe   # re-embed romfs (~1 min)
```
(If you changed a file compiled into the *cross* too, also `rm .yasos-build/cross.stamp`
and the whole thing rebuilds, ~8-10 min.)

---

## 4. Spot the cross's miscompile (disassembly)

Once you know function `F` (e.g. `ra_resolve_phis` in `ir/regalloc.c`), look at the
ARM the **cross** emits for it. The cross compiles each tinycc TU; reproduce that
exact compile and disassemble `F`:

```bash
cd libs/tinycc
# flags taken from the native build log line "armv8m-tcc -o armv8m-... -c ir/regalloc.c ..."
./bin/armv8m-tcc -o /tmp/F.o -c ir/regalloc.c \
  -DCONFIG_TCC_CROSSPREFIX='"armv8m-"' -I. -I./ir -I./ir/opt -DTCC_DEBUG=0 -g -O1 \
  -DTCC_ARM_VFP -DTCC_ARM_EABI=1 -DCONFIG_TCC_BCHECK=0 -DTCC_ARM_HARDFLOAT \
  -DTCC_TARGET_ARM_ARCHV8M -DTARGETOS_YasOS=1 -DTCC_TARGET_ARM_THUMB -DTCC_TARGET_ARM \
  -DTCC_IS_NATIVE -I$PWD/../../rootfs/usr/include -fpie -fPIE -mcpu=cortex-m33 \
  -fvisibility=hidden -std=c11 -Wno-declaration-after-statement
arm-none-eabi-objdump -dr /tmp/F.o | awk '/<F_NAME>:/{f=1} f{print} f&&/^$/{exit}'
```

**How to know which instruction is wrong** (you need a notion of "correct"):
- **Golden ARM reference**: compile the same TU with `arm-none-eabi-gcc -O1 -mcpu=cortex-m33`
  and diff the disassembly of `F`. Divergence that changes semantics = the cross bug.
- **Cross at -O0 vs -O1**: `./bin/armv8m-tcc -O0 -c …` vs `-O1`; the bug usually
  rides an optimization, so `-O0` shows the intended behavior.
- **Reason from source**: e.g. for `09` the wrong value implied a stale register
  read of an address-taken local across a call.

Known good-vs-bad patterns already found this way (all in MEMORY.md): dropped
`<<scale` on an indexed load/store, a MUL-const+ADD fusion leaving a partial
product, a register-VAR slot conflated with an anon stack local, a value cached
across a control-flow merge, **and a local whose address escaped to a call not
being reloaded after the call** (the `09` bug).

---

## 5. The `09` bug, end to end (concrete template)

- **Feature**: do-while only (`for`/`while` fine).
- **IR pass**: `ir/regalloc.c ra_resolve_phis`, the `target_count > 0` branch
  (~line 3168): a loop back-edge needing phi copies is rewritten from
  `JUMPIF(cond)→top` into `JUMPIF(!cond)→exit; <phi copies>; JUMP→top`.
- **Wrong computation**: the skip/exit target was stored as
  `skip_dest.u.imm32 = -(wp + 2)` **before** `ra_emit_scheduled_phi_copies(…,&wp,…)`
  advanced `wp`. `wp` is an **address-taken local** (`&wp` passed to the call).
- **Cross bug**: the cross cached `wp` in a register and did **not reload it after
  the call** for that one expression (the adjacent JUMP-write *did* reload it) →
  native used the stale pre-copies `wp` → exit target landed mid-body (IR 15) not
  the epilogue (18) → `bge 0xee` → infinite loop.
- **Fix (strategy A, source)**: move the skip-target store to **after** the JUMP
  write, using the now-fresh `wp`: `skip_dest.u.imm32 = -(wp + 1)`. Logically
  identical on the host; sidesteps the stale-register read on the device.
- The deeper cross bug (call not invalidating a cached address-taken local) is
  **latent** — strategy B would fix it for all callers.

---

## 6. Fix, then verify

**Strategy A (source workaround)** — edit `F`, rebuild (§3c), FAT-run the test:
the program must now behave (e.g. `09` prints `1..89` then `RC=0`; log ~400 B, not
~800 KB of runaway output).

**Strategy B (fix the cross)** — fix the cross's codegen/optimizer, `rm
.yasos-build/cross.stamp`, full rebuild, retest. This is preferred when the same
bug class blocks several tests: fix once, many tests pass.

**Always regression-test** — the official suite, reusing the current build:
```bash
./scripts/run_qemu_smoke.sh --no-build tcc_suite_test.py            # full suite
./scripts/run_qemu_smoke.sh --no-build tcc_suite_test.py -k 09_do_while   # one test
```
A regalloc/codegen fix can affect unrelated loops — run the whole suite, not just
the target.

---

## 7. Gotchas (each cost real time)

- **`pkill -f qemu-system-arm` SELF-KILLS your shell** — the pattern string is in
  the shell's own command line. Kill genuine QEMU by `comm`:
  `ps -eo pid,comm | awk '$2=="qemu-system-arm"{print $1}' | xargs -r kill -9`.
  Likewise never write `until ! pgrep -f qemu_fatdisk_run; do …` — the loop's own
  cmdline matches the pattern, so it never exits.
- **Stale ARM objects break the x86 cross link** — after a native build, the cross
  build fails with "file in wrong format". `rm -rf armv8m-arch armv8m-ir armv8m-*.o *.o`.
- **`config.mak` flips between cross and native** — `build_rootfs.sh` reconfigures
  each as needed; if building manually, reconfigure for the mode you want
  (`--enable-cross` for the cross).
- **Native rebuild is the slow loop** (~3-5 min) + kernel re-embed (~1 min). The
  device tcc (~2 MB) does **not** fit the 1 MB `/mnt` window, so you can't swap
  just the tcc binary — rebuild the romfs+kernel. Minimize native rebuilds: do all
  the host-side localization (§2, §3b, §4) first.
- **`-O0` native shifts the bug** — don't use it as a clean bisector.
- **`NATIVE_TCC_OPT_OVERRIDE`** env var (added to `build_rootfs.sh`) overrides the
  native opt level (default `-O1`) for experiments without editing the script.
- The bump commit is **not** automatically the cause — verify by reverting it; for
  `09`, reverting `e65f29d0` did not fix it (long-standing bug).

---

## 8. Checklist per test

1. FAT-run the failing test; capture device binary + behavior (§1).
2. Confirm host cross is correct → self-host (§2).
3. Narrow the feature (§3a), then the pass via `-dump-ir-passes=all` on a debug
   cross (§3b); if needed, instrument the pass for the device's actual values (§3c).
4. Disassemble `F` as the cross compiles it; find the wrong instruction vs a golden
   reference (§4).
5. Fix (A source workaround, or B cross codegen) (§6).
6. FAT-verify the test, then run the **full** smoke suite (§6).
7. Update MEMORY.md / the per-bug memory with root cause + fix.
