# ctags main/entry.c at -O1/-O2 loses the name a Kconfig `choice` takes from its `prompt`

**Status:** open · **Severity:** correctness (miscompile) · **Found:** 2026-09-28 (yasos ctags on QEMU, after the sccp fix 67aa75f6)

## Summary

universal-ctags (apps/ctags, pruned build) tagging this Kconfig:

```
menu "Kernel"
config SMP
	bool "SMP support"
choice
	prompt "Scheduler"
config SCHED_RR
	bool "round robin"
endchoice
endmenu
```

should rename the anonymous choice after its prompt (host ctags 6.2, and the
yasos build with every TU at -O0):

```
Scheduler	...	/^	prompt "Scheduler"$/;"	C	menu:Kernel
SCHED_RR	...	c	choice:Kernel""Scheduler	typeref:typename:bool
```

With `main/entry.c` compiled by tcc at -O1 or -O2 (all other TUs either level)
the tag keeps its anonymous name:

```
choice3228b3730104	...	/^choice$/;"	C	menu:Kernel
```

## Reproducer

TU bisection (every object -O0 except a subset at -O2, device run per step)
isolates `main/libctags_a-entry.o`: all -O0 + entry.o -O2 is BAD, all -O2 +
entry.o -O0 is OK. Still BAD with `-O1`, with `-O2 -fno-inline`, and with
**every** `TCC_DISABLE_PASS` knob of the 147 listed by `-dump-ir-passes=all`
disabled at once -- so the culprit is an ungated transform, codegen or the
register allocator, not one of the knobbed passes.

Build the pruned ctags as build_rootfs.sh's build_ctags() does, run on QEMU
via the FAT drive (`--language-force=Kconfig -f - /mnt/KCONFIG`).

The rename goes through the optscript DSL (optlib/kconfig.ctags: the prompt's
string replaces the choice tag's name); the entry.c side of that is presumably
the tag-name update on the cork queue entry -- a guess, not yet checked.

## Root cause

Not yet known. Next step: per-function mix of entry.c (-O0 vs -O2 bodies) to
find the function, then diff its -O0/-O2 code.

## Regression lock

None yet.

## Likely fix

Unknown until the function is found.
