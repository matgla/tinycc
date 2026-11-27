/* yasc modules: the module `geom`, imported by yasc_module_shapes.c and
 * yasc_module_main.c (test_qemu.py test_yasc_modules_run builds them in order). */
#pragma yasc
module geom;
#include <stddef.h>

export typedef struct point { int x, y; } point_t;
export typedef struct grid grid_t; /* opaque: its body stays here */
struct grid { int w, h; };
export enum dir { NORTH = 1, EAST = 2, SOUTH = 4 };

export int moves = 0;
static int calls; /* private: internal linkage */

int count(void) { return ++calls; } /* private too */

export size_t point_size(void) { count(); return sizeof(point_t); }
export point_t point_add(point_t a, point_t b)
{
  point_t r = {a.x + b.x, a.y + b.y};
  count();
  return r;
}
export inline int manhattan(point_t p) { return (p.x < 0 ? -p.x : p.x) + (p.y < 0 ? -p.y : p.y); }
export int geom_calls(void) { return calls; }

static grid_t the_grid = {8, 6};
export grid_t *borrows(static) grid_get(void) { return &the_grid; }
export int grid_cells(const grid_t *g) { return g->w * g->h; }
