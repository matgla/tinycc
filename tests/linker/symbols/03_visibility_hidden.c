/* __attribute__((visibility("hidden"))) must mark the symbol STV_HIDDEN
 * while remaining STB_GLOBAL (hidden is a visibility, not a binding).
 * A plain global without the attribute stays STV_DEFAULT.
 *
 * Note: the whole suite compiles with -fvisibility=hidden as a base cflag
 * (see _base_cflags() in test_linker.py), but that command-line flag is
 * currently NOT recognized by this fork's option parser (options_f table
 * in libtcc.c has no "visibility" entry), so it silently falls through to
 * "unsupported option" and has no effect. plain_global below stays
 * STV_DEFAULT even though -fvisibility=hidden is on the command line; only
 * the explicit attribute below actually produces STV_HIDDEN. This is a
 * front-end option-parsing gap, not a tccelf.c defect, so it is only
 * characterized here rather than reported as an ELF-writer bug. */
__attribute__((visibility("hidden"))) int hidden_var = 5;
__attribute__((visibility("hidden"))) int hidden_func(void) {
  return 4;
}

int plain_global = 6;
