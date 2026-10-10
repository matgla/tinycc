/* -fvisibility= and #pragma GCC visibility, as gcc applies them.
 *
 * -fvisibility= sets the visibility of every definition that names none of
 * its own; an attribute or an enclosing #pragma GCC visibility wins, the
 * pragma covers extern declarations too, and a symbol defined in top-level
 * asm keeps its own (gcc never hands -fvisibility to the assembler). */
int plain_var = 1;
int plain_func(void) { return plain_var; }
int tentative_var;
extern int later_def;
int later_def = 2;
int aliased_func(void) __attribute__((alias("plain_func")));

__attribute__((visibility("default"))) int attr_default(void) { return 3; }
__attribute__((visibility("protected"))) int attr_protected(void) { return 4; }

extern int undef_ref;
int use_undef(void) { return undef_ref; }

#pragma GCC visibility push(default)
int pragma_default(void) { return 5; }
__attribute__((visibility("hidden"))) int attr_beats_pragma(void) { return 6; }
#pragma GCC visibility push(hidden)
extern int pragma_hidden_ref;
int nested_hidden(void) { return pragma_hidden_ref; }
#pragma GCC visibility pop
int back_to_default(void) { return 7; }
#pragma GCC visibility pop

int after_pop(void) { return 8; }

__asm__(".globl asm_func\n.thumb_func\nasm_func: bx lr\n");
