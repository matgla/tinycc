/* The C11 _Pragma operator requires a parenthesized string-literal operand.
   A non-string operand must be rejected with a clear diagnostic rather than
   being silently mis-parsed as an ordinary expression. */
_Pragma(123)
int x = 1;
