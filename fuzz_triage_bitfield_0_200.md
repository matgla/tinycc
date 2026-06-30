# Fuzz O-level triage  (0-200)

Ground truth = `gcc -m32 -funsigned-char`.  tcc -O0 is normally correct.

| seed | class | ref | O0 | O1 | O2 | Os | culprit knob |
|------|-------|-----|----|----|----|----|--------------|
| 148 | O1 | 7f8a147b | 7f8a147b | 5dbe1cef | 7f8a147b | 7f8a147b | -fno-const-prop |

Repros in tests/fuzz/fuzz_triage_repros/.  Per-seed serial repro:
`python3 scripts/diff_olevels.py --seed N --require-qemu`
