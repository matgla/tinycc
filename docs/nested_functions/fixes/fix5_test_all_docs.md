# Task 5: Run `make test-all` and Document Final Results

**Depends on**: Fixes 1-4 applied
**Complexity**: Low (documentation only)

## Steps

### 1. Run full test suite

```bash
# Initialize GCC testsuite submodule if not already done
git submodule update --init --depth 1 tests/gcctestsuite/gcc-testsuite

# Run all tests
make test-all
```

### 2. Capture results

Record the final counts:
- Total compile tests passed/failed/skipped
- Total execute tests passed/failed/skipped
- Any new GCC torture tests that now pass (compared to current xfail list)

### 3. Update GCC xfail list if needed

In `tests/gcctestsuite/conftest.py`:
- If any tests in `GCC_XFAIL_TESTS` now pass, remove them from the xfail list
- If any new tests fail, investigate and either fix or add to xfail with reason

### 4. Update `docs/nested_functions/phase7_testing.md`

Move all 4 items from "Remaining (Known Limitations) 🚧" to "Completed ✅":

```markdown
### Completed ✅
// ... existing items ...
- [x] `nested_capture_array.c` — Array capture from parent (Fix 1: type propagation)
- [x] `nested_multi_level.c` — Multi-level nesting (Fix 4: chain-of-chains)
- [x] `nested_recursive_parent.c` — Recursive parent function (Fix 3: prescan filter)
- [x] `nested_struct_return.c` — Nested function returning struct (Fix 2: sret + types)
- [x] Run `make test-all` and document final GCC torture suite results
```

Update the test summary table:

```markdown
| Category | Passing | Failing | Status |
|----------|---------|---------|--------|
| Milestone 1 (Basic) | 3 | 0 | ✅ Complete |
| Milestone 2 (Capture) | 5 | 0 | ✅ Complete |
| Milestone 3 (Funcptr/Advanced) | 8 | 0 | ✅ Complete |
| GCC Torture (enabled) | 8+ | 0 | ✅ Complete |
| GCC Torture (skipped) | - | 6 | ⚪ Expected |
```

Add a "GCC Torture Suite Final Results" section with the `make test-all` output summary.

### 5. Verify clean test run

```bash
make test -j16       # IR tests — all pass, zero xfail
make test-all        # GCC torture — document results
make test-asm -j16   # Assembly tests — unaffected
```
