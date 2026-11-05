# tcc -c / link output: write errors are ignored, a truncated file returns 0

**Status:** open · **Severity:** medium · **Found:** 2026-11-04

## Summary

`tcc_output_file` never checks that its output reached the file. When a write
fails (full disk, file size limit, I/O error) tcc leaves a truncated object or
executable behind and exits 0. The failure only surfaces later, in whatever
reads the file next.

## Root cause

`source/obj/tccelf_output.c`, `tcc_output_file`: the writers (`tcc_output_elf`,
`tcc_output_binary`, `tcc_output_yaff`, and `fwrite` in `tccelf.c`) ignore
`fwrite`'s result, and `fclose(f)` is called with neither `ferror(f)` nor its own
return value looked at. `ret` therefore only reflects what the writers detect
themselves.

## Reproducer

A 2-block file size limit stands in for a full disk. A `/dev/full` symlink does
**not** work here: `tcc_output_file` unlinks the output first and creates a
regular file in its place.

```bash
printf 'char x[8192] = {1}; int f(void){return 1;}\n' > big.c
bash -c "trap '' XFSZ; ulimit -f 2; armv8m-tcc -c big.c -o big.o; echo rc=\$?; stat -c%s big.o"
# rc=0, 2048 bytes; the same command without the limit writes 8852 bytes
```

## Regression lock

None yet. `tcc -ar` now rejects a truncated object as an input
(`test_tcc_tool_ar_rejects_a_truncated_object`), which catches some of these
files downstream but not the wrong exit status.

## Likely fix

In `tcc_output_file`, after the writer returns: `if (fflush(f) || ferror(f))`
report the error and set a failing `ret`, check `fclose(f)` too, and let the
existing `unlink(filename)` remove the partial file. The stream's error flag is
sticky, so the individual `fwrite` calls need no checks of their own.
