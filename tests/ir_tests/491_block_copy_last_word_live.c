/* Codegen turns a run of frame word copies (LOAD a slot, STORE it to another,
   at consecutive offsets) into LDM/STM through scratch registers.  Each loaded
   value must die at its store; a run whose loads all use one register was
   taken as proof of that, since each load overwrites the last -- but nothing
   overwrites the run's last load.  Here load_cse forwards the copy's last two
   words (t37.body) to the by-value argument, so the last word's register is
   read after the copy, and the block copy left it holding something else.
   This is the shape of Zig's Air.Liveness.analyzeInstSwitchBr as the Zig C
   backend emits it; it and seven other functions of the tcc -O1 Zig compiler
   crashed. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
struct slice { uint32_t const *ptr; uintptr_t len; };
struct Case { uint32_t items[5]; struct slice body; };           /* 28 bytes: body at 20 */
struct OptCase { struct Case payload; bool is_null; };
struct It { uint32_t n; uint32_t data[2][7]; };
__attribute__((noinline)) struct OptCase next(struct It *it) {
  struct OptCase o = {0};
  if (it->n >= 2) { o.is_null = true; return o; }
  for (int i = 0; i < 5; i++) o.payload.items[i] = it->data[it->n][i];
  o.payload.body.ptr = &it->data[it->n][5]; o.payload.body.len = 2;
  it->n++;
  return o;
}
__attribute__((noinline)) uint32_t analyze(void *a, void *b, struct slice s) {
  uint32_t h = 0; for (uintptr_t i = 0; i < s.len; i++) h = h * 31 + s.ptr[i]; return h + (a == b);
}
__attribute__((noinline)) uint32_t walk(void *a0, void *a1, struct It *it) {
  struct OptCase t34;
  struct Case t36, t37;
  struct Case const *t38;
  struct slice const *t39;
  struct slice t40;
  uint32_t h = 0;
  for (;;) {
    t34 = next(it);
    if (t34.is_null) break;
    t36 = t34.payload;
    t37 = t36;
    t38 = (struct Case const *)&t37;
    t39 = (struct slice const *)&t38->body;
    t40 = (*t39);
    h = h * 7 + analyze(a0, a1, t40);
  }
  return h;
}
int main(void) {
  struct It it = {0, {{1, 2, 3, 4, 5, 6, 7}, {8, 9, 10, 11, 12, 13, 14}}};
  printf("%u\n", (unsigned)walk(&it, &it.n, &it));
  return 0;
}
