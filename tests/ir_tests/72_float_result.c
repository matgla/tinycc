/* Float test that returns result as integer */

int main() {
  float a = 1.0f;
  float b = 2.0f;
  float c = a + b;

  /* Convert float result to int to verify computation */
  int result = (int)c;  /* Should be 3 */
  
  if (result == 3) {
    return 1;  /* Success */
  }
  return 0;  /* Fail */
}
