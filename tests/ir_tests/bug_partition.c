extern void printf(const char *format, ...);

int array[4] = {30, 10, 20, 40};

void swap(int a, int b)
{
  int tmp = array[a];
  array[a] = array[b];
  array[b] = tmp;
}

int partition(int left, int right)
{
  int pivotIndex = left;
  int pivotValue = array[pivotIndex];
  int index = left;

  swap(pivotIndex, right);

  for (int i = left; i < right; i++)
  {
    if (array[i] < pivotValue)
    {
      swap(i, index);
      index += 1; // This increment is the problem!
    }
  }

  return index;
}

int main()
{
  printf("Array: %d %d %d %d\n", array[0], array[1], array[2], array[3]);
  int result = partition(0, 3);
  printf("Partition returned: %d\n", result);
  printf("Array: %d %d %d %d\n", array[0], array[1], array[2], array[3]);
  return 0;
}
