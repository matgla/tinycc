/* lib.vec: 2-D integer vectors -- a yasc module (see README.md).
 *
 * `module` names the module; its interface goes to lib/vec.ymi next to the
 * object.  Only what says `export` is visible to importers; everything else
 * here is private (internal linkage, as if static). */
module lib.vec;

export typedef struct vec2 { int x, y; } vec2_t;

export enum axis { AXIS_X, AXIS_Y };

/* An exported object: importers see `extern int vec_ops;`. */
export int vec_ops = 0;

/* Private: no importer can call it, and no other module's `count` clashes. */
static void count(void) { vec_ops++; }

export vec2_t vec_add(vec2_t a, vec2_t b)
{
  vec2_t r = {a.x + b.x, a.y + b.y};
  count();
  return r;
}

export vec2_t vec_scale(vec2_t v, int k)
{
  vec2_t r = {v.x * k, v.y * k};
  count();
  return r;
}

export int vec_get(vec2_t v, enum axis a) { return a == AXIS_X ? v.x : v.y; }

/* Importers get the body and inline it; C callers call the module's copy. */
export inline int vec_dot(vec2_t a, vec2_t b) { return a.x * b.x + a.y * b.y; }
