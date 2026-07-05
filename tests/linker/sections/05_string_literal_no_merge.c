/* Two identical string literals are NOT deduplicated: tcc emits a fresh
 * local symbol/copy of the bytes for each literal, and .rodata is a plain
 * PROGBITS section without the SHF_MERGE|SHF_STRINGS flags gcc would use
 * for a mergeable string section. This is a simplification, not a spec
 * violation (mergeable string sections are optional), so it is locked in
 * here as current behavior rather than reported as a defect. */
const char *msg_a = "hello";
const char *msg_b = "hello";
