/* Two identical string literals ARE deduplicated: the frontend keeps a
 * content-keyed pool per translation unit, so the same bytes are emitted once
 * in rodata and both references share that single copy (C11 6.4.5p7 permits
 * identical literals to share storage). Each literal still has its own anon
 * symbol, but both point at the same offset; no orphan symbol is left behind.
 * .rodata stays a plain PROGBITS section (no SHF_MERGE|SHF_STRINGS) because
 * dedup happens in the compiler, not via a mergeable string section. */
const char *msg_a = "hello";
const char *msg_b = "hello";
