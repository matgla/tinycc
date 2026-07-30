# Backend Architecture Restructure

## Goal

Move all target-dependent backend code into `source/backend/arch/` so that
future architectures (x86, RISC-V, etc.) can be added alongside ARM/Thumb
without polluting the top-level tree.

## Current Layout

```
arch/
  arm/
    arm.c, arm.h, arm_aapcs.c, arm_regalloc.c, arm_regalloc.h, ssa_opt_arm.c
    thumb/
      thumb.c, thumb.h, thop_*.c/h (40+ opcode builders)
    Makefile
  fpu/
    arm/fpv5-d16.c/h, fpv5-sp-d16.c/h
    Makefile
  Makefile

arm-thumb-gen.c, arm-thumb-asm.c, arm-thumb-callsite.c, arm-thumb-scratch.c,
arm-thumb-defs.h, thumb-tok.h, arm-link.c  -- scattered at top level
```

## Target Layout

```
source/backend/arch/
  Makefile                    # top-level dispatcher
  arm/
    Makefile                  # ARM arch + dispatches to thumb/
    arm.c, arm.h, arm_aapcs.c
    arm_regalloc.c, arm_regalloc.h
    ssa_opt_arm.c, ssa_opt_arm.h
    thumb/
      Makefile
      thumb.c, thumb.h
      thop_*.c/h (40+ opcode builders)
      arm-thumb-gen.c, arm-thumb-asm.c, arm-thumb-callsite.c, arm-thumb-scratch.c
      arm-thumb-defs.h, thumb-tok.h
    arm-link.c
  fpu/
    arm/
      fpv5-d16.c/h
      fpv5-sp-d16.c/h
    Makefile
  <future: riscv/, x86/, ...>
```

## Why This Structure

1. **One place per architecture.** `source/backend/arch/<arch>/` is the only
   place the build system needs to touch to add or remove an arch.
2. **Mirror the `source/opt/` hierarchy.** Just as `source/opt/ssa/` groups
   all SSA passes, `source/backend/arch/arm/thumb/` groups all Thumb opcodes.
3. **No top-level clutter.** The root of the repo stays clean — only
   `tccgen.c`, `tccelf.c`, `tccld.c`, `tccdbg.c`, `libtcc.c` and the
   `ir/` subsystem live at the top level.
4. **Future-proof.** Adding RISC-V means adding `source/backend/arch/riscv/`
   with its own `Makefile`, opcode subdirectory, and test suite. The
   top-level `arch/Makefile` dispatcher becomes the `source/backend/Makefile`.

## File Inventory (move to `source/backend/arch/`)

### arch/arm/ → source/backend/arch/arm/
| Source | Destination |
|--------|-------------|
| `arch/arm/arm.c` | `source/backend/arch/arm/arm.c` |
| `arch/arm/arm.h` | `source/backend/arch/arm/arm.h` |
| `arch/arm/arm_aapcs.c` | `source/backend/arch/arm/arm_aapcs.c` |
| `arch/arm/arm_regalloc.c` | `source/backend/arch/arm/arm_regalloc.c` |
| `arch/arm/arm_regalloc.h` | `source/backend/arch/arm/arm_regalloc.h` |
| `arch/arm/ssa_opt_arm.c` | `source/backend/arch/arm/ssa_opt_arm.c` |
| `arch/arm/ssa_opt_arm.h` | `source/backend/arch/arm/ssa_opt_arm.h` |
| `arch/arm/Makefile` | `source/backend/arch/arm/Makefile` |

### arch/arm/thumb/ → source/backend/arch/arm/thumb/
| Source | Destination |
|--------|-------------|
| `arch/arm/thumb/thumb.c` | `source/backend/arch/arm/thumb/thumb.c` |
| `arch/arm/thumb/thumb.h` | `source/backend/arch/arm/thumb/thumb.h` |
| `arch/arm/thumb/thop_*.c` | `source/backend/arch/arm/thumb/thop_*.c` |
| `arch/arm/thumb/thop_*.h` | `source/backend/arch/arm/thumb/thop_*.h` |
| `arch/arm/thumb/Makefile` | `source/backend/arch/arm/thumb/Makefile` |
| `arm-thumb-gen.c` (top-level) | `source/backend/arch/arm/thumb/arm-thumb-gen.c` |
| `arm-thumb-asm.c` (top-level) | `source/backend/arch/arm/thumb/arm-thumb-asm.c` |
| `arm-thumb-callsite.c` (top-level) | `source/backend/arch/arm/thumb/arm-thumb-callsite.c` |
| `arm-thumb-scratch.c` (top-level) | `source/backend/arch/arm/thumb/arm-thumb-scratch.c` |
| `arm-thumb-defs.h` (top-level) | `source/backend/arch/arm/thumb/arm-thumb-defs.h` |
| `thumb-tok.h` (top-level) | `source/backend/arch/arm/thumb/thumb-tok.h` |

### arch/fpu/ → source/backend/arch/fpu/
| Source | Destination |
|--------|-------------|
| `arch/fpu/arm/fpv5-d16.c` | `source/backend/arch/fpu/arm/fpv5-d16.c` |
| `arch/fpu/arm/fpv5-d16.h` | `source/backend/arch/fpu/arm/fpv5-d16.h` |
| `arch/fpu/arm/fpv5-sp-d16.c` | `source/backend/arch/fpu/arm/fpv5-sp-d16.c` |
| `arch/fpu/arm/fpv5-sp-d16.h` | `source/backend/arch/fpu/arm/fpv5-sp-d16.h` |
| `arch/fpu/Makefile` | `source/backend/arch/fpu/Makefile` |

### Top-level → source/backend/arch/arm/
| Source | Destination |
|--------|-------------|
| `arm-link.c` | `source/backend/arch/arm/arm-link.c` |

## What Stays Where

- `arch/Makefile` → deleted (replaced by `source/backend/Makefile`)
- `source/backend/generators/` → stays as-is (function.c, regalloc.c)
- `ir/` subsystem → stays as-is
- `tccgen.c`, `tccelf.c`, `tccld.c`, `tccdbg.c`, `libtcc.c` → stay at top level
- `arch/` directory → deleted after all files are moved

## Makefile Changes

### Top-level Makefile
- Remove `arch/arm`, `arch/fpu` from `ARCH_OBJS` / `ARCH_LIBS`
- Add `source/backend/arch/arm`, `source/backend/arch/fpu` to `ARCH_OBJS`
- Update include paths: `-I$(TOP)/source/backend/arch/arm` replaces `-I$(TOP)/arch/arm`
- Remove `arch/thumb` from `THUMB_OBJS`
- Add `source/backend/arch/arm/thumb` to `THUMB_OBJS`
- Remove `arch/arm/thumb` from `ARM_THUMB_OBJS`
- Add `source/backend/arch/arm/thumb` to `ARM_THUMB_OBJS`

### source/backend/Makefile (new)
- Top-level dispatcher: `make -C arch/arm`, `make -C arch/fpu`
- Follows same pattern as `arch/Makefile`

### source/backend/arch/arm/Makefile (new)
- Builds `libarm.a` from arm.c, arm_aapcs.c, arm_regalloc.c, ssa_opt_arm.c
- Dispatches to `arch/arm/thumb/` for libthumb.a
- Includes `libarm-link.a` for arm-link.o

### source/backend/arch/arm/thumb/Makefile (new)
- Builds `libthumb.a` from thumb.c + all thop_*.c
- Also builds arm-thumb-gen.c, arm-thumb-asm.c, arm-thumb-callsite.c, arm-thumb-scratch.c

### source/backend/arch/fpu/Makefile (new)
- Builds `libfpu.a` from fpv5-d16.c, fpv5-sp-d16.c

### source/backend/generators/Makefile
- Update include path: `-I$(TOP)/source/backend/arch/arm` for `arm_regalloc.h`

### tests/unit/arm/armv8m/Makefile
- Update all include paths (`-I$(TOP)/source/backend/arch/...`)
- Update all source references for moved files

## Include Path Changes

| Old include | New include |
|-------------|-------------|
| `arch/arm/arm.h` | `source/backend/arch/arm/arm.h` |
| `arch/arm/arm_regalloc.h` | `source/backend/arch/arm/arm_regalloc.h` |
| `arch/arm/arm_aapcs.c` | `source/backend/arch/arm/arm_aapcs.c` |
| `arch/arm/ssa_opt_arm.c` | `source/backend/arch/arm/ssa_opt_arm.c` |
| `arch/arm/thumb/thumb.h` | `source/backend/arch/arm/thumb/thumb.h` |
| `arch/arm/thumb/thop_*.h` | `source/backend/arch/arm/thumb/thop_*.h` |
| `arm-thumb-defs.h` | `source/backend/arch/arm/thumb/arm-thumb-defs.h` |
| `thumb-tok.h` | `source/backend/arch/arm/thumb/thumb-tok.h` |
| `arch/fpu/arm/fpv5-d16.h` | `source/backend/arch/fpu/arm/fpv5-d16.h` |

## Test Suite

- `tests/unit/arm/armv8m/` — all existing tests continue to work unchanged
- `tests/thumb/armv8m/` — all existing tests continue to work unchanged
- No new tests needed — this is a pure restructure

## Rollback Plan

If anything breaks, `git stash` the Makefile changes and `git checkout` the
moved files back to their original locations. The git history preserves
everything.

## Execution Order

1. Create directory structure
2. Move files (git mv preserves history)
3. Update Makefiles (top-level first, then sub-Makefiles)
4. Update all `#include` directives across the codebase
5. Build and verify `make cross`
6. Run `make test` to confirm nothing regressed

## Open Questions

- [ ] Should `arm-link.c` live in `source/backend/arch/arm/` or
      `source/backend/`? Currently it's a top-level TU.
- [ ] Should `source/backend/generators/function.c` include
      `source/backend/arch/arm/arm_regalloc.h` directly, or go through a
      `source/backend/arch/arm/include/` indirection?
- [ ] Does the `arch/` Makefile dispatcher pattern need to be preserved, or
      can we collapse to a single `source/backend/Makefile`?
