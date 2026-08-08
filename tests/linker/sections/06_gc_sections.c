/* gc_sections behaviour: what must vanish and what must survive.
 *
 * Linked as a .o with -Wl,--gc-sections -e entry, so the loader's
 * keep-text-split path and the linker's section GC are both exercised --
 * the same path the self-hosted device tcc link takes.
 */

/* Never referenced: the whole body must be collected. */
static int gc_dead_static(int x) { return x * 37 + 5; }

/* Referenced only through a dispatch table in .data: the relocation chain
 * .text.entry -> .data -> .text.gc_addr_taken must keep it alive. */
static int gc_addr_taken(int x) { return x * 3 + 1; }

static int (*gc_table[1])(int) = {gc_addr_taken};

/* Unreferenced but default-visibility global: a runtime root (other yasld
 * modules may resolve it), must survive without any witnessing reloc. */
int gc_exported_unused(int x) { return x + 100; }

/* Hidden global, unreferenced: invisible outside the image, collectable. */
__attribute__((visibility("hidden"))) int gc_hidden_unused(int x) { return x - 7; }

int entry(int v) { return gc_table[0](v); }
