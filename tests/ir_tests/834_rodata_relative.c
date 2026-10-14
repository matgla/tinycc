/* __rodata_relative pointers: the word holds an offset into .rodata, so a
 * const table of them carries no load-time relocation (docs/rodata_relative.md).
 * Reads through every kind of access must give back the real pointer. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REL __rodata_relative

typedef struct
{
  const char *REL name;
  int value;
  const char *REL alias; /* NULL for most */
} Entry;

static const Entry table[] = {
    {"alpha", 1, NULL},
    {"beta", 2, "b"},
    {"gamma", 3, NULL},
    {"delta", 4, "d"},
    {NULL, 0, NULL},
};

static const char *REL const words[] = {"one", "two", "three", NULL};

/* Not const: still offsets, the object just lives in writable data. */
static Entry mutable_table[] = {{"m0", 10, NULL}, {"m1", 11, "mm"}};

static const char blob[] = "0123456789";
static const int numbers[] = {7, 8, 9};

static const struct
{
  const char *REL p; /* into the middle of an object */
  const int *REL q;  /* not char */
} inner = {blob + 4, &numbers[2]};

typedef const char *REL relstr;
static const relstr typedefd[] = {"td0", "td1"};

static int cmp_entry(const void *a, const void *b)
{
  const Entry *ea = a, *eb = b;
  return strcmp(ea->name, eb->name);
}

static const Entry *find(const char *name)
{
  for (const Entry *e = table; e->name; e++)
    if (!strcmp(e->name, name))
      return e;
  return NULL;
}

/* Through a pointer to the element: the type still says relative. */
static const char *first_word(const char *REL const *w)
{
  return *w;
}

static int count_words(void)
{
  int n = 0;
  while (words[n])
    n++;
  return n;
}

volatile int idx = 2;

int main(void)
{
  int i;
  for (i = 0; table[i].name; i++)
    printf("%d %s %d %s\n", i, table[i].name, table[i].value, table[i].alias ? table[i].alias : "-");

  printf("words=%d last=%s\n", count_words(), words[count_words() - 1]);
  printf("idx word=%s len=%d\n", words[idx], (int)strlen(words[idx]));
  printf("char=%c\n", *table[idx].name);
  printf("char2=%c\n", table[1].name[2]);

  const Entry *e = find("delta");
  printf("find=%d %s\n", e ? e->value : -1, e && e->alias ? e->alias : "-");
  printf("find missing=%d\n", find("zeta") == NULL);

  /* A copy of the struct keeps meaning the same strings. */
  Entry copy = table[1];
  printf("copy=%s %s\n", copy.name, copy.alias);
  Entry copies[2];
  memcpy(copies, &table[2], sizeof copies);
  printf("memcpy=%s %s\n", copies[0].name, copies[1].alias);

  printf("first=%s\n", first_word(&words[1]));
  printf("inner=%s %d\n", inner.p, *inner.q);
  printf("typedef=%s %s\n", typedefd[0], typedefd[1]);
  printf("mutable=%s %s\n", mutable_table[1].name, mutable_table[1].alias);

  /* NULL tests and comparisons. */
  printf("null=%d %d\n", table[0].alias == NULL, table[1].alias != NULL);
  printf("eq=%d\n", table[3].alias == table[3].alias);
  const char *p = table[0].name;
  printf("arith=%s %s\n", p + 2, table[2].name + 1);

  /* bsearch through void *: the comparator reads the fields. */
  Entry sorted[4];
  memcpy(sorted, table, sizeof sorted);
  qsort(sorted, 4, sizeof sorted[0], cmp_entry);
  for (i = 0; i < 4; i++)
    printf("sorted %s\n", sorted[i].name);
  Entry key;
  memcpy(&key, &table[2], sizeof key); /* "gamma" */
  const Entry *hit = bsearch(&key, sorted, 4, sizeof sorted[0], cmp_entry);
  printf("bsearch=%s %d\n", hit ? hit->name : "-", hit ? hit->value : -1);

  printf("sizeof=%d %d\n", (int)sizeof(table[0].name), (int)sizeof table);
  return 0;
}
