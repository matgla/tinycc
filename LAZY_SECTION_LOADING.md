# Lazy Section Loading Implementation

## Summary

Successfully implemented deferred section loading for ELF object files and archives, reducing memory usage during compilation by loading section data on-demand rather than at load time.

## Changes Made

### 1. TCCState Structure (tcc.h)

Added `current_archive_path` field to track the archive file path separately from member names:

```c
/* Archive file path for lazy loading (NULL if not in archive) */
const char *current_archive_path;
```

### 2. Section Structure (tcc.h)

Extended Section structure with lazy loading fields:

```c
typedef struct DeferredChunk {
  const char *source_path;      /* File/archive path (duplicated) */
  uint32_t file_offset;         /* Offset within file */
  uint32_t size;                /* Size of chunk */
  uint32_t dest_offset;         /* Destination offset in section */
  int materialized;             /* Per-chunk tracking */
  struct DeferredChunk *next;
} DeferredChunk;

typedef struct Section {
  /* ... existing fields ... */
  int lazy;                     /* Section uses lazy loading */
  int materialized;             /* Section data is loaded */
  int has_deferred_chunks;      /* Has deferred chunks */
  DeferredChunk *deferred_head; /* Linked list of chunks */
  DeferredChunk *deferred_tail;
  /* ... rest of fields ... */
} Section;
```

### 3. Core Functions (tccelf.c)

#### `should_defer_section()`
Decides which sections to defer:
- Always defers DWARF debug sections (`.debug_*`)
- Never defers relocation sections (needed by GC)
- Never defers ARM exception handling (`.ARM.*`)
- Never defers `.eh_frame` (needed for stack unwinding)
- Defers all other sections (`.text`, `.data`, `.rodata`, `.bss`)

#### `section_add_deferred()`
Records a chunk for lazy loading:
- Duplicates the source path string
- Creates a DeferredChunk and adds it to the section's list
- Marks section as lazy

#### `section_materialize()`
Loads all deferred chunks for a section:
- Opens the source file
- Reads each chunk at the recorded offset
- Writes to the section's data buffer
- Applies any relocation patches
- Marks section as materialized

#### `section_ensure_loaded()`
Checks if materialization is needed before access

#### `free_deferred_chunks()`
Frees deferred chunk metadata including duplicated paths

### 4. Archive Loading (tccelf.c)

Updated `tcc_load_alacarte()` and `tcc_load_archive()`:
- Save and restore `current_archive_path`
- Set `current_archive_path` before loading archive members
- Use archive path (not member name) for deferred chunks

### 5. Object File Loading (tccelf.c)

Updated `tcc_load_object_file()`:
- When `should_defer_section()` returns true, record chunk instead of loading
- For archives, compute absolute offset including archive member offset

### 6. Relocation (tccelf.c)

Updated `relocate_section()`:
- Always materialize non-debug sections before relocation
- For debug sections, store patches instead of materializing

## Test Results

All 466 tests pass with lazy section loading enabled.

## Memory Benefits

Section data is loaded only when needed:
- At link time when relocations are applied
- At output time when writing the ELF file
- Debug sections can remain unloaded if not needed

## Design Notes

1. **String Duplication**: Source paths are duplicated when creating deferred chunks to survive after loading context changes.

2. **Per-Chunk Tracking**: Each chunk has its own `materialized` flag for potential partial materialization.

3. **Archive Handling**: Archive file paths are tracked separately from member names to ensure correct file access during materialization.

4. **Debug Sections**: Special handling for DWARF debug sections to support relocation streaming without full materialization.
