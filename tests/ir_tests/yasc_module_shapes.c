/* yasc modules: the module `shapes`, which imports `geom` (see yasc_module_geom.c). */
#pragma yasc
module shapes;
import geom;

export typedef struct rect { point_t lo, hi; } rect_t;

export int rect_area(rect_t r)
{
  moves++;
  return (r.hi.x - r.lo.x) * (r.hi.y - r.lo.y);
}
export rect_t rect_shift(rect_t r, point_t d)
{
  rect_t s = {point_add(r.lo, d), point_add(r.hi, d)};
  moves++;
  return s;
}
export static inline enum dir rect_side(void) { return SOUTH; }
