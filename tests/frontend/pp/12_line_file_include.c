/* __LINE__ must reset relative to each file (1 at the top of the included
   header, then resume counting in the includer after the #include returns),
   and must reflect the *use* site when expanded from inside a function-like
   macro body, not the macro's definition site.
   Note: __FILE__'s value is intentionally not printed here -- the test
   harness invokes the compiler with an absolute path to this very file, so
   asserting on __FILE__'s exact text would bake the repo checkout's
   absolute filesystem path into the golden and break on any other clone
   location. The #ifdef below only checks that __FILE__ is a recognized,
   always-defined macro inside an included file (not just the main file). */
int main_line1 = __LINE__;
#include "line_hdr.h"
int main_line3 = __LINE__;
#define WRAP_LINE() __LINE__
int wrapped = WRAP_LINE();
#ifdef __FILE__
int file_macro_defined = 1;
#endif
