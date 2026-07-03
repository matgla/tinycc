/* BUG (regression pin): the `_Pragma(string-literal)` unary operator
   (C11 6.10.9) is not implemented at all in tccpp.c -- there is no
   TOK__Pragma / keyword recognition anywhere in the lexer, only the
   `#pragma` *directive* form is handled (pragma_parse() in tccpp.c).
   Per the standard, `_Pragma("X")` must be destringized and processed as
   if by `#pragma X` right there in the token stream (this is what lets
   `#define DO_PRAGMA(x) _Pragma(#x)` conditionally emit pragmas from
   macros -- a very common portable-header idiom).
   Current (wrong) behavior: under `-E`, the `_Pragma(...)` call is passed
   through completely untouched instead of being rewritten to
   `#pragma message "hi"` (compare: gcc -E performs the rewrite). This
   golden pins that passthrough. The effect is worse than a cosmetic -E
   difference: a real (non -E) compile of `_Pragma("message \"hi\"")`
   fails outright, e.g. at file scope with
   `error: identifier expected`, or inside a function body with
   `warning: implicit declaration of function '_Pragma'` followed by
   `error: ';' expected`, because `_Pragma` is parsed as an ordinary
   (unrecognized) identifier/call rather than a preprocessor operator.
   Once `_Pragma` support is added, this golden must be updated to the
   destringized-and-rewritten form. */
_Pragma("message \"hi\"")
int x = 1;
