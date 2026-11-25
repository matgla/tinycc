/* The regular-C half of the -fyasc-facts run test (yasc_facts_cases.c). */
#include <stdio.h>

struct P
{
  int a, b, c, d;
};

static int said;

int use(const struct P *p) { return p->a * 1000 + p->b * 100 + p->c * 10 + p->d; }
void fill(struct P *p, int v) { p->a = v, p->b = v + 1, p->c = v + 2, p->d = v + 3; }
void bump(struct P *p) { p->b += 11; }
int say(int x) { said += x; return said & 7; }

int case_read_then_call(int x);
int case_written(int x);
int case_partly_written(int x);
int case_loop(int n);
int case_two(int x);
int case_offset(int x);

struct B
{
  int len, cap, data[8];
};
int case_restrict_reload(struct B *b, int *other, int n);
int case_restrict_array(int *d, int *s, int n);
int case_restrict_self(int *d, int n);
int case_restrict_derived(int *d, int *s, int n);
int case_restrict_runtime_offset(int *d, int k, int n);
int case_fresh(int *other, int n);
int case_restrict_peeked(int *d, int *s, int n);
int peek(const int *p) { return p[4] + 1; }
int peeki(const int *p) { return *p * 3 + 1; }
void puti(int *p, int v) { *p = v; }
void bumpi(int *p) { *p += 5; }
long long peekll(const long long *p) { return *p + 12345; }
int case_scalar_read(int n);
int case_scalar_out(int n);
int case_scalar_loop(int n);
int case_scalar_path(int n);
int case_scalar_twice(int n);
int case_scalar_ll(int n);
int case_dead_after(int x);
int case_store_between(int x);
int case_store_loop(int n);
int case_const_in_loop(int n);
int case_written_in_loop(int n);
int case_named_write_in_loop(int n);
int case_licm_field(struct B *b, int *dst, int n);
int case_licm_self(struct B *b, int *dst, int n);
int case_licm_derived(int *d, int *dst, int k, int n);
int case_licm_handed(int *d, int *dst, int n);

int main(void)
{
  printf("read_then_call %d\n", case_read_then_call(3));
  printf("written %d\n", case_written(3));
  printf("partly_written %d\n", case_partly_written(3));
  printf("loop %d\n", case_loop(5));
  printf("two %d\n", case_two(4));
  printf("offset %d\n", case_offset(9));
  struct B b = {1, 2, {0}};
  int other[4] = {0}, d[8] = {0, 1, 2, 3, 4, 5, 6, 7}, s[4] = {0};
  printf("restrict_reload %d %d\n", case_restrict_reload(&b, other, 9), other[2]);
  printf("restrict_array %d %d\n", case_restrict_array(d, s, 5), s[1]);
  printf("restrict_self %d %d\n", case_restrict_self(d, 40), d[3]);
  printf("restrict_derived %d %d %d\n", case_restrict_derived(d, s, 6), d[3], s[0]);
  printf("restrict_runtime_offset %d %d\n", case_restrict_runtime_offset(d, 3, 77), d[3]);
  printf("fresh %d %d\n", case_fresh(other, 4), other[1]);
  printf("restrict_peeked %d %d\n", case_restrict_peeked(d, s, 8), s[2]);
  printf("scalar_read %d\n", case_scalar_read(4));
  printf("scalar_out %d\n", case_scalar_out(21));
  printf("scalar_loop %d\n", case_scalar_loop(6));
  printf("scalar_path %d %d\n", case_scalar_path(2), case_scalar_path(9));
  printf("scalar_twice %d\n", case_scalar_twice(7));
  printf("scalar_ll %d\n", case_scalar_ll(3));
  printf("dead_after %d\n", case_dead_after(6));
  printf("store_between %d\n", case_store_between(5));
  printf("store_loop %d\n", case_store_loop(4));
  printf("const_in_loop %d\n", case_const_in_loop(5));
  printf("written_in_loop %d\n", case_written_in_loop(5));
  printf("named_write_in_loop %d\n", case_named_write_in_loop(5));
  {
    struct B lb = {3, 7, {0}};
    int dd[8] = {0, 1, 2, 3, 4, 5, 6, 7}, ds[4] = {0};
    printf("licm_field %d %d\n", case_licm_field(&lb, ds, 6), ds[1]);
    printf("licm_self %d %d %d\n", case_licm_self(&lb, ds, 5), ds[2], lb.cap);
    printf("licm_derived %d %d %d\n", case_licm_derived(dd, ds, 1, 4), ds[3], dd[1]);
    printf("licm_handed %d %d %d\n", case_licm_handed(dd, ds, 4), ds[0], dd[0]);
  }
  printf("said %d\n", said);
  return 0;
}
