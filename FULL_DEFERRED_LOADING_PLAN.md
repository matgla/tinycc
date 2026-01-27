# Full Deferred Section Loading - Implementation Plan

## Status: ⚠️ PARTIALLY IMPLEMENTED - NEEDS DEBUGGING

**Date:** 2026-01-30  
**Issue:** Implementation causes 308 test failures (158 pass). The infrastructure is in place but there's a bug in the materialization logic.

## Implementation Progress

### ✅ Completed

1. **Data Structure Changes (tcc.h)**
   - Added `materialized` field to `DeferredChunk` struct
   - Added `has_deferred_chunks` and `fully_materialized` to `Section` struct

2. **Core Functions (tccelf.c)**
   - Updated `should_defer_section()` to defer all non-relocation sections
   - Modified `section_add_deferred()` to initialize new fields
   - Updated `section_materialize()` for per-chunk tracking
   - Updated `section_ensure_loaded()` to use new flags
   - Updated all callers to use new flag names

### ❌ Issues

**Test Results:**
- Without changes: 466 passed
- With changes: 158 passed, 308 failed

**Suspected Root Cause:**
The issue is likely in how mixed sections (existing sections receiving additional data from object files) are handled. When `.text` is created during `tccelf_new()` and then receives data from object files, we need to:
1. Reserve space in the section
2. Not allocate/initialize the buffer
3. Load chunks on-demand

The problem might be that `section_realloc()` is not being called correctly, or the section buffer is being accessed before materialization.

## Implementation Details

### Data Structures

```c
/* Deferred chunk with materialization tracking */
typedef struct DeferredChunk {
    const char *source_path;
    uint32_t file_offset;
    uint32_t size;
    uint32_t dest_offset;
    struct DeferredChunk *next;
    int materialized;        /* NEW: Per-chunk tracking */
} DeferredChunk;

/* Section with deferred loading state */
typedef struct Section {
    /* ... existing fields ... */
    int lazy;
    int materialized;        /* Legacy - kept for compatibility */
    int has_deferred_chunks; /* NEW: Has unloaded chunks */
    int fully_materialized;  /* NEW: All chunks loaded */
    DeferredChunk *deferred_head;
    DeferredChunk *deferred_tail;
    /* ... */
} Section;
```

### Key Functions

#### should_defer_section()
```c
static int should_defer_section(const char *name, int sh_type)
{
    /* Always defer DWARF debug sections */
    if (strncmp(name, ".debug_", 7) == 0)
        return 1;
    
    /* Never defer relocation sections */
    if (sh_type == SHT_REL || sh_type == SHT_RELA)
        return 0;
    
    /* Never defer ARM exception handling */
    if (strncmp(name, ".ARM", 4) == 0)
        return 0;
    
    /* Never defer eh_frame */
    if (strncmp(name, ".eh_frame", 9) == 0)
        return 0;
    
    /* Defer everything else */
    return 1;
}
```

#### section_materialize()
```c
ST_FUNC void section_materialize(TCCState *s1, Section *sec)
{
    DeferredChunk *c;
    int fd;

    if (!sec->has_deferred_chunks || sec->fully_materialized)
        return;

    /* Allocate buffer for full section */
    if (sec->sh_type != SHT_NOBITS) {
        section_realloc(sec, sec->data_offset);
        /* Note: NOT zeroing buffer to preserve compiled data */
    }

    /* Load each non-materialized chunk */
    for (c = sec->deferred_head; c; c = c->next) {
        if (c->materialized)
            continue;
        
        fd = open(c->source_path, O_RDONLY | O_BINARY);
        if (fd < 0) {
            fprintf(stderr, "tcc: cannot reopen '%s'\n", c->source_path);
            continue;
        }
        
        lseek(fd, c->file_offset, SEEK_SET);
        if (full_read(fd, sec->data + c->dest_offset, c->size) != c->size) {
            fprintf(stderr, "tcc: short read from '%s'\n", c->source_path);
        }
        close(fd);
        c->materialized = 1;
    }
    
    /* Apply relocation patches */
    if (sec->nb_reloc_patches > 0) {
        apply_reloc_patches(sec, sec->data, sec->data_offset);
    }

    free_deferred_chunks(sec);
    sec->fully_materialized = 1;
    sec->has_deferred_chunks = 0;
}
```

## Debugging Checklist

- [ ] Verify `section_realloc()` preserves existing data (mixed sections)
- [ ] Verify `section_ensure_loaded()` is called before all data access
- [ ] Check if output streaming works correctly for deferred sections
- [ ] Verify relocation sections are NOT deferred (needed for GC)
- [ ] Check if ARM sections are properly excluded from deferral

## Next Steps

1. Add debug output to track section materialization
2. Compare memory layout between baseline and deferred loading
3. Verify section data is identical at output time
4. Check for off-by-one errors in chunk loading

## Expected Memory Savings

If implemented correctly:
- **Before:** ~708 KB peak (hello_world)
- **After:** ~400 KB peak
- **Savings:** ~300 KB (deferred library sections)

## Files Modified

- `tcc.h`: DeferredChunk and Section struct updates
- `tccelf.c`: Core deferred loading logic

## Backwards Compatibility

- No API changes
- No command-line changes
- Output should be identical to immediate loading
