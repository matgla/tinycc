/* Test various float operations */

int main() {
  float a = 10.0f;
  float b = 3.0f;
  
  /* Test add */
  float sum = a + b;  /* 13.0 */
  if ((int)sum != 13) return 0;
  
  /* Test sub */
  float diff = a - b;  /* 7.0 */
  if ((int)diff != 7) return 0;
  
  /* Test mul */
  float prod = a * b;  /* 30.0 */
  if ((int)prod != 30) return 0;
  
  /* Test div */
  float quot = a / b;  /* 3.333... truncates to 3 */
  if ((int)quot != 3) return 0;
  
  /* All tests passed */
  return 1;
}
