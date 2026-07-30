/* 64-bit register-deref pairing: aligned typed derefs must use LDRD/STRD,
 * packed-derived accesses must stay on the unaligned-safe LDR/STR pair
 * (LDRD/STRD fault on unaligned addresses on ARMv7-M/v8-M). */

long long ll_load(long long *p) { return *p; }

void ll_store(long long *p, long long v) { *p = v; }

double d_load(double *p) { return *p; }

struct __attribute__((packed)) PK
{
  char c;
  long long v;
  long long a[2];
};

long long pk_load(struct PK *s) { return s->v; }

void pk_store(struct PK *s, long long x) { s->v = x; }

long long pk_arr(struct PK *s, int i) { return s->a[i]; }

void pk_arr_store(struct PK *s, int i, long long x) { s->a[i] = x; }
