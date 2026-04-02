#include <stdio.h>

#include "../benchmarks/benchmarks.h"

void register_benchmark(const char *name, benchmark_func_t func, int iterations, const char *description)
{
}

void register_benchmark_ex(const char *name, benchmark_func_t func, int iterations, const char *description,
                           int expected_result)
{
}

#include "../benchmarks/mibench_adapters/mibench_dijkstra.c"
#include "../benchmarks/mibench_adapters/mibench_qsort.c"
#include "../benchmarks/mibench_adapters/mibench_rijndael.c"
#include "../benchmarks/mibench_adapters/mibench_stringsearch.c"

int main(void)
{
  int dijkstra = bench_mibench_dijkstra(64);
  int qsort = bench_mibench_qsort(200);
  int rijndael = bench_mibench_rijndael(300);
  int stringsearch = bench_mibench_stringsearch(300);

  printf("dijkstra = %d\n", dijkstra);
  printf("qsort = %d\n", qsort);
  printf("rijndael = %d\n", rijndael);
  printf("stringsearch = %d\n", stringsearch);

  if (dijkstra == 199 && qsort == 54258 && rijndael == 18890 && stringsearch == 351) {
    printf("PASS\n");
    return 0;
  }

  printf("FAIL\n");
  return 1;
}
