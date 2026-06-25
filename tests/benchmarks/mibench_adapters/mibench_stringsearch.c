/*
 * MiBench Stringsearch Adapter for RP2350 Benchmark Suite
 *
 * Reuses the Pratt-Boyer-Moore search logic with embedded search cases.
 */

#include "../benchmarks.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

static size_t stringsearch_table[UCHAR_MAX + 1];
static size_t stringsearch_len;
static const char *stringsearch_pattern;

static const char *const find_strings[] = {
    "field",
    "regime",
    "impact",
    "texture",
    "phase",
    "images",
    "conductor",
    "proper",
    NULL,
};

static const char *const search_strings[] = {
    "In recent years, the field of photonic crystals has found new applications in RF systems.",
    "A new type of metallic regime is often used to discuss electromagnetic structures.",
    "The new surface treatment is having a significant impact on antenna behavior.",
    "A conductive surface covered with special texture alters electromagnetic properties.",
    "It does not reverse the phase of reflected waves in the selected band.",
    "The effective image currents appear in-phase in several practical images.",
    "Surface waves do not propagate on a normal conductor in this synthetic paragraph.",
    "An important question as to the proper nature and scope of University involvement remains.",
};

static void init_stringsearch(const char *pattern)
{
  stringsearch_len = strlen(pattern);
  for (size_t index = 0; index <= UCHAR_MAX; index++)
  {
    stringsearch_table[index] = stringsearch_len;
  }
  for (size_t index = 0; index < stringsearch_len; index++)
  {
    stringsearch_table[(unsigned char)pattern[index]] = stringsearch_len - index - 1;
  }
  stringsearch_pattern = pattern;
}

static char *run_stringsearch(const char *text)
{
  size_t shift = 0;
  size_t pos = stringsearch_len - 1;
  size_t limit = strlen(text);

  while (pos < limit)
  {
    while (pos < limit)
    {
      shift = stringsearch_table[(unsigned char)text[pos]];
      if (shift == 0)
      {
        break;
      }
      pos += shift;
    }

    if (pos < limit && shift == 0)
    {
      char *match = (char *)&text[pos - stringsearch_len + 1];
      if (strncmp(stringsearch_pattern, match, stringsearch_len) == 0)
      {
        return match;
      }
      pos++;
    }
  }

  return NULL;
}

int bench_mibench_stringsearch(int iterations)
{
  int checksum = 0;

  for (int iteration = 0; iteration < iterations; iteration++)
  {
    checksum = 0;
    for (int index = 0; find_strings[index] != NULL; index++)
    {
      char *match;

      init_stringsearch(find_strings[(index + iteration) & 7]);
      match = run_stringsearch(search_strings[(index + iteration) & 7]);
      if (match != NULL)
      {
        checksum += (int)(match - search_strings[(index + iteration) & 7]);
        checksum += (int)strlen(find_strings[(index + iteration) & 7]);
      }
    }
  }

  return checksum;
}

void init_mibench_stringsearch(void)
{
  register_benchmark_ex("mibench_stringsearch", bench_mibench_stringsearch, 300, "MiBench: String search", 351);
}
