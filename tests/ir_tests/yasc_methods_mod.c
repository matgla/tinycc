/* yasc methods in a module: an exported struct's methods are exported with
 * it, under their plain symbols TAG__NAME; a private struct's stay private.
 * Built by test_qemu.py test_yasc_methods_module_run, imported by
 * yasc_methods_mod_main.c. */
#pragma yasc
module shapes2;

export struct rect
{
  int w, h;

  struct rect make(int w, int h) { return (struct rect){w, h}; }
  int area(const struct rect * view self) { return self->w * self->h + self->pad(); }
  /* importers get the body: they may inline it */
  inline int perimeter(const struct rect * view self) { return 2 * (self->w + self->h); }
  static inline int is_square(const struct rect * view self) { return self->w == self->h; }
  void grow(inout struct rect * view self, int by)
  {
    self->w += by;
    self->h += by;
  }
  int pad(const struct rect * view self) { return 0 * self->w; }
};

struct tally
{
  int n;
  void add(inout struct tally * view self, int k) { self->n += k; }
};

static struct tally total;

export int shapes2_total(const struct rect * view r)
{
  total.add(r->area());
  return total.n;
}
