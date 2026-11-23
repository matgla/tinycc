/* shapes: rectangles on top of lib.vec -- a module importing a module.
 * An importer of shapes gets lib.vec too. */
module shapes;

import lib.vec;

export typedef struct rect { vec2_t lo, hi; } rect_t;

export rect_t rect_make(int x0, int y0, int x1, int y1)
{
  rect_t r = {{x0, y0}, {x1, y1}};
  return r;
}

export int rect_area(rect_t r)
{
  vec2_t d = vec_add(r.hi, vec_scale(r.lo, -1));
  return d.x * d.y;
}

export rect_t rect_move(rect_t r, vec2_t by)
{
  rect_t m = {vec_add(r.lo, by), vec_add(r.hi, by)};
  return m;
}

/* No symbol: the body goes to importers (and into the C header). */
export static inline int rect_width(rect_t r) { return r.hi.x - r.lo.x; }
