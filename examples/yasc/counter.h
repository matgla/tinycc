/* counter.h -- a small C library, written as C libraries are (chapter 6).
 * Regular C: yasc reads its pointers as unchecked, except where its
 * attributes say more -- counter_new's result is owned, and counter_free
 * takes it back. */
#ifndef COUNTER_H
#define COUNTER_H

typedef struct counter counter_t;

void counter_free(counter_t *c);
counter_t *counter_new(const char *name) __attribute__((malloc, malloc(counter_free, 1), nonnull(1)));
void counter_add(counter_t *c, int n);
int counter_get(const counter_t *c);
const char *counter_name(const counter_t *c); /* points into the counter */

#define COUNTER_STEP 5
#define COUNTER_NONE ((counter_t *)0)

#endif
