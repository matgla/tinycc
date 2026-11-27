/* yasc methods (docs/yasc_methods.md): a function declared in a struct body
 * is the file-scope function TAG__NAME; `obj.m()` passes &obj, `p->m()`
 * passes p, a by-value self gets a copy, a method without self is called as
 * TAG.m().  Bodies see the whole struct and every method, whatever the order. */
#pragma yasc
extern int printf(const char *, ...);

struct vec
{
  int x, y;

  int len2(const struct vec *self) { return self->x * self->x + self->y * self->y; }

  void scale(inout struct vec *self, int k)
  {
    self->x *= k;
    self->y *= k;
  }

  /* by value: the callee gets a copy */
  struct vec add(struct vec self, struct vec o)
  {
    self.x += o.x;
    self.y += o.y;
    return self;
  }

  /* no self: vec.make(...) */
  struct vec make(int x, int y) { return (struct vec){x, y}; }

  /* calls a method declared further down */
  int dot(const struct vec *self, const struct vec *o)
  {
    return self->x * o->x + self->y * o->y + self->zero();
  }

  static int zero(const struct vec *self) { return self->x - self->x; }

  static inline int sum(const struct vec *self) { return self->x + self->y; }

  /* declared here, defined below under its symbol name */
  int area(const struct vec *self);
};

int vec__area(const struct vec *self) { return self->x * self->y; }

typedef struct vec vec_t;

union num
{
  int i;
  unsigned u;
  int is_negative(const union num *self) { return self->i < 0; }
};

struct counter
{
  int n;
  struct counter *view bump(inout struct counter *self)
  {
    self->n++;
    return self;
  }
  void reset(void) { }
};

typedef struct counter counter_t;

static int apply(int (*f)(const struct vec *), const struct vec *v) { return f(v); }

static struct vec make_global(void) { return vec.make(7, 1); }

struct vec g = {2, 5};

int main(void)
{
  struct vec v = vec.make(3, 4);
  struct vec *view p = &v;
  printf("len2 %d\n", v.len2());
  p->scale(2);
  printf("scaled %d %d %d\n", v.x, v.y, p->len2());
  struct vec w = v.add(vec_t.make(1, 1));
  printf("add %d %d (v %d %d)\n", w.x, w.y, v.x, v.y);
  struct vec w2 = p->add(w);
  printf("add2 %d %d\n", w2.x, w2.y);
  printf("dot %d\n", w.dot(&v));
  printf("sum %d area %d\n", v.sum(), v.area());
  printf("tag call %d symbol %d\n", vec.len2(&w), vec__len2(&w));
  printf("rvalue %d\n", vec.make(1, 2).len2());
  printf("call result %d\n", make_global().sum());
  printf("global %d\n", g.len2());
  printf("pointer %d\n", apply(vec.len2, &v));

  union num n = {-5};
  printf("union %d %d\n", n.is_negative(), (union num){7}.is_negative());

  counter_t c = {0};
  c.bump()->bump()->bump();
  counter_t.reset();
  printf("counter %d\n", c.n);

  struct vec arr[2] = {{1, 1}, {2, 3}};
  int total = 0;
  for (int i = 0; i < 2; i++)
    total += arr[i].len2();
  printf("array %d\n", total);
  return 0;
}
