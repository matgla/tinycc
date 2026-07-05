/* _Pragma produced by macro expansion -- the whole point of the operator
   (C11 6.10.9).  The `DO_PRAGMA' idiom stringizes its argument with `#x'
   and feeds it to `_Pragma', so the operator must be recognized *after*
   macro expansion (in next(), not only next_nomacro()).  Each expanded
   `_Pragma(...)' must be rewritten under -E to a `#pragma ...' line, with
   the destringized `\"' turning back into `"'.  Verified against gcc -E. */
#define DO_PRAGMA(x) _Pragma(#x)
DO_PRAGMA(message "from macro")
DO_PRAGMA(GCC diagnostic ignored "-Wunused")
int a = 1;
