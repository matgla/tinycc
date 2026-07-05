/* Pasting an identifier/number with an adjacent punctuator that does not
   recombine into a single valid preprocessing token (C11 6.10.3.3p3: "If the
   result is not a valid preprocessing token, the behavior is undefined").
   tcc recovers by re-lexing the pasted text as however many tokens it
   naturally splits into, emits a "does not give a valid preprocessing
   token" warning, and keeps going -- this is a permitted (if idiosyncratic)
   recovery strategy for UB, not a standard violation. This test pins tcc's
   current recovery output (including its formatting quirks) so a future
   change to the recovery path is a deliberate, visible decision. */
#define PLUSPLUS(a) a ## ++
int i = 1;
int j = PLUSPLUS(i);
#define NEG(a) - ## a
int k = 5;
int m = NEG(3);
