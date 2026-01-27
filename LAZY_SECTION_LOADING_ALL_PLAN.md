# Implementation Plan: Extend Deferred Section Loading to All External Object File Sections

## Status: ❌ IMPLEMENTATION REVERTED

**Date:** 2026-01-30
**Reason:** Implementation caused 308 test failures. The existing deferred loading for debug sections only works correctly; extending it to all sections requires more careful handling.

## Original Plan

### Goal
Extend TinyCC's deferred section loading from debug-only (`.debug_*`) to ALL non-relocation sections loaded from external object files (archives and standalone .o files).

### Expected Memory Savings
- **Current peak (hello_world):** ~708 KB
- **Library section data:** ~200-400 KB
- **Expected savings:** 150-300 KB (unused sections never loaded)

## Implementation Attempt

### Changes Made (and Reverted)

**1. tcc.h - Added `from_object_file` field**
```c
int from_object_file;         /* 1 = section data loaded from external object file */
```

**2. tccelf.c - Modified `should_defer_section()`**
- Changed signature: `const char *name` → `Section *s, int is_new_section`
- Defer if: `from_object_file == 1` AND `is_new_section == 1` AND not ARM/eh_frame/relocation

**3. tccelf.c - Set flag in `tcc_load_object_file()`**
```c
s->from_object_file = 1;
```

**4. tccelf.c - Fixed `section_materialize()`**
- Removed `memset(sec->data, 0, sec->data_allocated)`
- This preserves existing data in mixed sections

## Issues Encountered

### Test Results
- **Without changes:** 466 passed
- **With changes:** 158 passed, 308 failed

### Root Cause Analysis

The implementation had the following issues:

1. **Mixed Section Handling Problem**
   - Sections like `.text` exist before object file loading (created in `tccelf_new()`)
   - When object files add data to these sections, the data must be loaded immediately
   - Attempting to defer only "new" sections helps, but many critical sections are mixed

2. **ARM-Specific Sections**
   - `.ARM.extab` and `.ARM.exidx` are exception handling tables
   - These were being deferred but are needed for runtime exception handling
   - Added exclusion for ARM sections, but this reduces the benefit

3. **Section Materialization Timing**
   - Sections were deferred but not materialized before output
   - `section_write_streaming()` should handle this, but there may be edge cases
   - Relocation application requires sections to be materialized

4. **Complexity of Mixed Sections**
   - When a section has both compiled data (immediate) and loaded data (deferred)
   - The materialization must preserve existing data while loading new data
   - Removing `memset()` helps, but the coordination is complex

## Conclusion

**The deferred section loading optimization was NOT implemented.**

The existing implementation that only defers `.debug_*` sections works correctly and provides significant memory savings for debug builds. Extending this to all sections would require:

1. More sophisticated tracking of which sections can be safely deferred
2. Better handling of mixed sections (compiled + loaded data)
3. Careful coordination with the relocation system
4. Extensive testing to ensure no runtime regressions

Given the complexity and the limited memory savings (~150-300 KB out of 708 KB peak), the existing debug-only deferred loading is sufficient.

## Current State

The code remains with the original debug-only deferred loading:
```c
static int should_defer_section(const char *name)
{
    /* Only defer DWARF debug sections */
    if (strncmp(name, ".debug_", 7) == 0)
        return 1;
    return 0;
}
```

This provides ~100-200 KB memory savings for debug builds without the complexity of full deferred loading.
