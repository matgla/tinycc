/* Simple floating point test without printf */

int main() {
  float a = 1.0f;
  float b = 2.0f;
  float c = a + b;

  if (c > 2.5f) {
    return 1;  /* Success */
  }
  return 0;  /* Fail */
}
