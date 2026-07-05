/* The C11 6.10.9 `_Pragma(string-literal)' unary operator.
   `_Pragma("X")' is destringized (outer quotes stripped, \" -> " and
   \\ -> \) and processed as if `#pragma X' had appeared at that point in
   the token stream.  Under `-E' it must therefore be rewritten to a
   `#pragma X' line on its own -- matching `gcc -E'.  (Previously tccpp.c
   had no TOK__Pragma recognition at all and passed `_Pragma(...)' through
   verbatim, which also made a real compile of it fail.)
   This golden pins the rewrite; see 19_pragma_operator_macro.c for the
   macro-expansion (`DO_PRAGMA') idiom. */
_Pragma("message \"hi\"")
int x = 1;
