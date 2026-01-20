# DWARF Debug Info with -ffunction-sections Support

## Problem Summary

### Reported Symptom

When code is compiled with `-ffunction-sections`, `--gc-sections`, `-g`, and `-gdwarf`, GDB shows incorrect source lines. This typically happens because the line program and address ranges still assume a single `.text` section, while code is split into per-function sections and some of those sections may be discarded by garbage collection.

When `-ffunction-sections` is enabled, GDB cannot show line numbers because:
1. Each function goes into its own `.text.funcname` section instead of `.text`
2. DWARF debug info generation assumes all code is in a single `.text` section
3. `tcc_debug_line()` skips line info when `cur_text_section != text_section`
4. Address ranges in `.debug_aranges` and `.debug_info` reference empty `.text` section
5. Line number program uses addresses relative to `.text` start (0), but code is elsewhere
6. With `--gc-sections`, some `.text.*` sections are discarded, but debug ranges still reference them

## Implementation Plan

### Phase 1: Track Multiple Text Sections

- [x] **1.1** Add data structure to track all text sections with code
  - Location: `tccdbg.c` or `tcc.h`
  - Add array/list to `dwarf_line` or new struct to track:
    - Section pointer
    - Start offset in line program
    - Symbol for relocations

- [x] **1.2** Register text sections when functions are generated
  - Location: `tccgen.c` in function generation code
  - When `cur_text_section` changes, register it for debug tracking

### Phase 2: Fix Line Number Generation

- [x] **2.1** Remove `cur_text_section != text_section` check in `tcc_debug_line()`
  - Location: `tccdbg.c:1803`
  - Replace with check that section is executable/valid

- [x] **2.2** Remove same check in `tcc_debug_line_num()`
  - Location: `tccdbg.c:1874`

- [x] **2.3** Track current section in line number state
  - Add `dwarf_line.cur_section` field
  - Emit `DW_LNE_set_address` when section changes
  - Reset `dwarf_line.last_pc` on section change

- [x] **2.4** Generate section-relative addresses with proper relocations
  - Each `DW_LNE_set_address` needs relocation to correct section
  - Use section symbol instead of global `section_sym`
  - Emit `DW_LNE_set_address` on every section change and at the start of the line program
  - Ensure line program never mixes addresses from different sections without a reset

### Phase 3: Fix Address Ranges (.debug_aranges)

- [x] **3.1** Generate multiple address range entries
  - Location: `tccdbg.c` in `tcc_debug_end()`
  - One entry per text section that has code
  - Each entry needs: start address (reloc), length

- [x] **3.2** Calculate proper section sizes
  - Track `data_offset` for each registered text section
  - Use actual section sizes, not `text_section->data_offset`
  - Skip sections with zero size or that are discarded by `--gc-sections`

### Phase 4: Fix Compilation Unit Info (.debug_info)

- [x] **4.1** Update `DW_AT_low_pc` and `DW_AT_high_pc`
  - Location: `tccdbg.c` in `tcc_debug_start()` and `tcc_debug_end()`
  - Option A: Use lowest/highest addresses across all sections
  - Option B: Use ranges (DW_AT_ranges) instead of low/high PC

- [x] **4.2** Consider using DW_AT_ranges for DWARF 4+
  - More accurate for non-contiguous code
  - Requires `.debug_ranges` or `.debug_rnglists` section
  - Filter out discarded sections so CU ranges match the final linked image

### Phase 5: Fix Function Debug Info

- [x] **5.1** Update `tcc_debug_funcstart()`
  - Location: `tccdbg.c`
  - Use `cur_text_section` for function's section
  - Generate proper relocations

- [x] **5.2** Update `tcc_debug_funcend()`
  - Location: `tccdbg.c`
  - Calculate function size from section, not global `ind`

### Phase 6: Testing

- [x] **6.1** Create test case: simple function with `-ffunction-sections -g`
- [ ] **6.2** Verify `readelf --debug-dump=line` shows correct file/line mapping
- [ ] **6.3** Verify `readelf --debug-dump=aranges` shows all code ranges
- [ ] **6.4** Test with GDB: `info line main`, `list main`, breakpoints
- [ ] **6.5** Test linked ELF (not just .o file)
- [ ] **6.6** Run existing test suite to check for regressions

## Key Files to Modify

1. **tccdbg.c** - Main debug info generation
   - `tcc_debug_line()` - Line number generation
   - `tcc_debug_line_num()` - Line number generation (IR mode)
   - `tcc_debug_start()` - Compilation unit start
   - `tcc_debug_end()` - Compilation unit end, aranges
   - `tcc_debug_funcstart()` - Function debug info
   - `tcc_debug_funcend()` - Function debug info

2. **tcc.h** - Data structures
   - `dwarf_line` struct - Add section tracking

3. **tccgen.c** - Code generation
   - Function generation - Register sections for debug

## Data Structure Changes

```c
// In tcc.h or tccdbg.c

struct dwarf_text_section {
    Section *section;      // The .text.funcname section
    int sym_index;         // Symbol for relocations
    int line_prog_start;   // Offset in line program where this section starts
};

// Add to dwarf_line or create new struct
struct {
    // ... existing fields ...
    Section *cur_section;  // Currently active text section
    struct dwarf_text_section *sections;  // Array of text sections
    int n_sections;
    int max_sections;
} dwarf_line;
```

## DWARF Line Program Changes

When section changes, emit:
```
DW_LNE_set_address <address with relocation to new section>
```

This resets the state machine's address register to the start of the new section.

## References

- DWARF 4/5 specification for line number program
- `readelf --debug-dump=line` for debugging output
- GCC's handling of `-ffunction-sections` with `-g` for reference

## Estimated Complexity

- Phase 1-2: Medium - Core tracking and line info
- Phase 3-4: Medium - Address ranges and CU info
- Phase 5: Low - Function info updates
- Phase 6: Low - Testing

Total: ~200-400 lines of code changes
