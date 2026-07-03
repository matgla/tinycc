# Fuzz O-level triage  (100-500)

Ground truth = `gcc -m32 -funsigned-char`.  tcc -O0 is normally correct.

| seed | class | ref | O0 | O1 | O2 | Os | culprit knob |
|------|-------|-----|----|----|----|----|--------------|
| 218 | O2 | ? | 569064ef | 569064ef | bae58432 | 569064ef | - |
| 408 | O1 | ? | cfd9ee9c | 49a476ae | cfd9ee9c | cfd9ee9c | - |
| 465 | O1 | ? | 604fac3c | 27cc71ea | 604fac3c | 604fac3c | - |

Repros in tests/fuzz/fuzz_triage_repros/.  Per-seed serial repro:
`python3 scripts/diff_olevels.py --seed N --require-qemu`
