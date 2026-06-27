# Fuzz O-level triage  (2000-10000)

Ground truth = `gcc -m32 -funsigned-char`.  tcc -O0 is normally correct.

FIXED 2026-06-29: 2137 (store-src LEA hoist, test 210) + 8425 (same); 2657
(load_cse runtime stack-indexed store, test 211); 2698 (cprop copy-into-loop-phi
lost-copy, test 212) + 5689,8300,8606 (same root cause). 2137/2657 committed
d528cd9d; 2698-batch uncommitted.

| seed | class | ref | O0 | O1 | O2 | Os | culprit knob |
|------|-------|-----|----|----|----|----|--------------|
| 2137 | FIXED | 794b3b5f | 794b3b5f | 794b3b5f | 794b3b5f | 794b3b5f | -fno-loop-unroll |
| 2657 | FIXED | 4a152f38 | 4a152f38 | 4a152f38 | 4a152f38 | 4a152f38 | -fno-const-prop |
| 2698 | FIXED | 157ae9b8 | 157ae9b8 | 157ae9b8 | 157ae9b8 | 157ae9b8 | -fno-dead-store-elim (trigger; root=cprop) |
| 2874 | FIXED | 75fc991c | 75fc991c | 75fc991c | 75fc991c | 75fc991c | -fno-const-prop (trigger; root=store_redundant const-idx) |
| 3210 | O1 | a720d0d4 | a720d0d4 | 2c0f55a4 | 2c0f55a4 | a720d0d4 | -fno-const-prop |
| 3691 | O1 | be32b4b4 | be32b4b4 | fdfbf84e | fdfbf84e | be32b4b4 | - |
| 4193 | O1 | 72294662 | 72294662 | bd139bc6 | 72294662 | 72294662 | - |
| 4482 | O1 | e1357798 | e1357798 | cc8b501a | e1357798 | e1357798 | -fno-const-prop |
| 4594 | O1 | 85f333ee | 85f333ee | bd949db6 | 85f333ee | 85f333ee | - |
| 5656 | O1 | 6d65f022 | 6d65f022 | 62a2b208 | 62a2b208 | 6d65f022 | -fno-const-prop |
| 5689 | FIXED | 363ab459 | 363ab459 | 363ab459 | 363ab459 | 363ab459 | (fixed by 2698 cprop lost-copy) |
| 6214 | O1 | d32fd859 | d32fd859 | 7e62f31c | 7e62f31c | d32fd859 | -fno-const-prop |
| 6447 | O1 | 155dfd09 | 155dfd09 | f5e3daaa | f5e3daaa | 155dfd09 | -fno-const-prop |
| 6951 | O2 | 97fc3b2c | 97fc3b2c | 97fc3b2c | 639b943e | 97fc3b2c | -fno-jump-threading |
| 7918 | O1 | 643d852b | 643d852b | 0995ec15 | 643d852b | 643d852b | - |
| 8078 | COMPILE_CRASH | e39fd06b | e39fd06b | e39fd06b | COMPILE_FAIL | e39fd06b | - |
| 8300 | FIXED | 1fa8a66e | 1fa8a66e | 1fa8a66e | 1fa8a66e | 1fa8a66e | (fixed by 2698 cprop lost-copy) |
| 8425 | FIXED | c1c7764c | c1c7764c | c1c7764c | c1c7764c | c1c7764c | (fixed by 2137 store-src LEA hoist) |
| 8606 | FIXED | 9574ca19 | 9574ca19 | 9574ca19 | 9574ca19 | 9574ca19 | (fixed by 2698 cprop lost-copy) |
| 8985 | O2 | 43b26518 | 43b26518 | 43b26518 | 77968521 | 43b26518 | -fno-loop-unroll |
| 9403 | O1 | b24aea33 | b24aea33 | 21f95921 | b24aea33 | b24aea33 | -fno-const-prop |

Repros in tests/fuzz/fuzz_triage_repros/.  Per-seed serial repro:
`python3 scripts/diff_olevels.py --seed N --require-qemu`
