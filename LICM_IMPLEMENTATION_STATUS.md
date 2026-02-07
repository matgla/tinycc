# LICM Implementation Status

## Summary

Loop-Invariant Code Motion (LICM) has been partially implemented. The basic infrastructure works but has known bugs that prevent enabling it by default.

## What Was Implemented

### ✅ Phase 1: Loop Detection (Simplified)
- File: `ir/licm.c`, `ir/licm.h`
- Algorithm: Pattern-based detection of backward jumps
- Detects natural loops from JUMP instructions targeting lower addresses
- Tracks loop body instructions

### ✅ Phase 2: Loop-Invariant Identification
- Identifies `Addr[StackLoc[offset]]` operands as invariant
- Marks ADD instructions with stack address operands as candidates

### ✅ Phase 3: Code Hoisting
- Creates ASSIGN instructions to copy invariant values
- Inserts hoisted code before loop header
- Handles instruction index shifting

### ✅ Phase 4: Integration
- Added `opt_licm` flag to TCCState
- Integrated into optimization pipeline in `tccgen.c`
- Added to build system (Makefile)

## Known Issues

### 🐛 Bug: Function Call Parameter Tracking
When LICM inserts instructions, it shifts subsequent instructions, breaking call_id tracking for function parameters.

**Symptom:**
```
error: compiler_error: missing FUNCPARAMVAL for call_id=0 arg=0
```

**Root Cause:** 
The IR uses `call_id` to track function call sequences. When we insert an instruction, the call_id metadata gets misaligned because:
1. CALLSEQ_BEGIN assigns a call_id
2. LICM inserts instruction (shifting indices)
3. CALLARG tries to use the shifted call_id
4. Mismatch causes the error

### 🐛 Bug: Incomplete Operand Replacement
The hoisted value is created but not always used in the loop body. The original instruction still computes the address directly.

**Example:**
```c
// Hoisted: R1(T5) <-- Addr[StackLoc[-256]]
// But loop body still uses: Addr[StackLoc[-256]] ADD R2
// Instead of: T5 ADD R2
```

## Current Status

| Component | Status | Notes |
|-----------|--------|-------|
| Loop Detection | ✅ Working | Simplified but functional |
| Invariant ID | ✅ Working | Stack addresses identified |
| Hoisting | ⚠️ Partial | Inserts code but has bugs |
| Integration | ⚠️ Disabled | Flag exists but not enabled |
| Function Calls | ❌ Broken | Cannot handle call sequences |

## Test Results

### Bubble Sort Test
- **Without LICM:** 574 bytes
- **With LICM:** 574 bytes (no change)
- **Reason:** Operand replacement not fully working

### Loop Invariant Test
- **Without LICM:** Passes
- **With LICM:** Fails with function call error

## Usage

To enable LICM for testing:
```bash
./armv8m-tcc -O1 -flicm -c test.c
```

Note: Only safe for simple loops without function calls.

## Next Steps to Fix

1. **Fix call_id tracking:** Update call_id metadata when inserting instructions
2. **Fix operand replacement:** Ensure all uses of hoisted value are updated
3. **Add safety checks:** Skip hoisting if function calls are in the loop
4. **Testing:** Add comprehensive test cases

## Files Modified

- `ir/licm.c` - New file with LICM implementation
- `ir/licm.h` - New header with API
- `Makefile` - Added LICM to build
- `tcc.h` - Added `opt_licm` flag
- `libtcc.c` - Added flag (currently disabled)
- `tccgen.c` - Integrated LICM pass
