/* --gc-sections when the plain .text input section is entirely dead.
 *
 * Everything live sits in .text.<name> (-ffunction-sections); the only
 * thing in plain .text is never referenced, so GC collects .text itself.
 * coalesce_function_sections then folds the surviving .text.<name>
 * sections into that same text_section -- which GC used to strip of
 * SHF_ALLOC, so layout dropped every live function: with a linker script
 * the image had no code at all, without one layout_sections segfaulted on
 * an image with no PT_LOAD.  The yasboot -O0 link hit it through runtime
 * archive members that only dead zig.h helpers referenced.
 */

/* Plain .text, unreferenced: GC must collect it. */
__attribute__((section(".text"))) int gc_dead_in_plain_text(int x) { return x * 7 + 3; }

/* Pinned by the script's KEEP(*(.vectors)), like a vector table. */
__attribute__((section(".vectors"), used)) const unsigned gc_vectors[2] = {0x20001000u, 0u};

static int gc_live_helper(int v) { return v + 1; }

int _start(int v) { return gc_live_helper(v); }
