/* imports yasc_methods_mod.c (module shapes2): its struct's methods */
#pragma yasc
import shapes2;
extern int printf(const char *, ...);

int main(void)
{
  struct rect r = rect.make(3, 4);
  struct rect *view p = &r;
  printf("area %d perimeter %d square %d\n", r.area(), p->perimeter(), r.is_square());
  p->grow(1);
  printf("grown %d %d %d\n", r.w, r.h, rect__area(&r));
  printf("total %d\n", shapes2_total(&r));
  printf("total %d\n", shapes2_total(&r));
  return 0;
}
