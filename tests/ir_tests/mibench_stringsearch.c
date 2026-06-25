/* MiBench Stringsearch - regression detection for -O2
 * Pratt-Boyer-Moore string pattern matching.
 */
#include <stdio.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

static size_t search_table[UCHAR_MAX + 1];
static size_t search_len;
static const char *search_pattern;

static const char *const find_strings[] = {
    "field",     "regime",    "impact",    "texture",
    "phase",     "images",    "conductor", "proper",
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

static void init_search(const char *pattern)
{
  search_len = strlen(pattern);
  for (size_t index = 0; index <= UCHAR_MAX; index++) {
    search_table[index] = search_len;
  }
  for (size_t index = 0; index < search_len; index++) {
    search_table[(unsigned char)pattern[index]] = search_len - index - 1;
  }
  search_pattern = pattern;
}

static char *run_search(const char *text)
{
  size_t shift = 0;
  size_t pos = search_len - 1;
  size_t limit = strlen(text);

  while (pos < limit) {
    while (pos < limit) {
      shift = search_table[(unsigned char)text[pos]];
      if (shift == 0) break;
      pos += shift;
    }

    if (pos < limit && shift == 0) {
      char *match = (char *)&text[pos - search_len + 1];
      if (strncmp(search_pattern, match, search_len) == 0) {
        return match;
      }
      pos++;
    }
  }

  return NULL;
}

int bench_mibench_stringsearch(void)
{
  int checksum = 0;
  int iterations = 300;

  for (int iteration = 0; iteration < iterations; iteration++) {
    checksum = 0;
    for (int index = 0; find_strings[index] != NULL; index++) {
      char *match;

      init_search(find_strings[(index + iteration) & 7]);
      match = run_search(search_strings[(index + iteration) & 7]);
      if (match != NULL) {
        checksum += (int)(match - search_strings[(index + iteration) & 7]);
        checksum += (int)strlen(find_strings[(index + iteration) & 7]);
      }
    }
  }

  return checksum;
}

int main(void)
{
    printf("stringsearch: %d\n", bench_mibench_stringsearch());
    return 0;
}
