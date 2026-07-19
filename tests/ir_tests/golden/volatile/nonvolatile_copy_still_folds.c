/* Guard against over-correction: a plain (non-volatile) local initialised to a
 * constant must still be folded away — the volatile guards must key strictly on
 * the volatile flag, never on "is a local VAR". */
int nonvolatile_copy_still_folds(void)
{
  int x = 5;
  int y = x + 3;
  return y;
}
