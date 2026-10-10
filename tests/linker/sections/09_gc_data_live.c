/* Per-object data under --gc-sections, the live half: entry's own dispatch
 * table in this object's .data keeps live_target alive.  Linked together
 * with 09_gc_data_dead.c, whose .data nobody references. */

static int live_target(int x) { return x * 5 + 2; }

static int (*live_table[1])(int) = {live_target};

int entry(int v) { return live_table[0](v); }
