/* Per-object data under --gc-sections, the dead half: nothing references
 * this object's tables, so their targets must be collected with them.  The
 * loader used to merge every object's .data/.rodata into one section, so
 * the live table in 09_gc_data_live.c kept all of this alive. */

static int dead_via_data(int x) { return x * 11 + 3; }
static int dead_via_rodata(int x) { return x * 13 + 4; }

__attribute__((visibility("hidden"))) int (*dead_table[1])(int) = {dead_via_data};
__attribute__((visibility("hidden"))) int (*const dead_rotable[1])(int) = {dead_via_rodata};
__attribute__((visibility("hidden"))) int dead_counter;
