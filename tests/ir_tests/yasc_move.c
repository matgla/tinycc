/* yasc move(p) / borrow(p): the value is the pointer itself, at every -O. */
#pragma yasc
extern int printf(const char * view, ...);
void * owned maybe malloc(unsigned long n);
void free(drop void * owned maybe p);

struct box { char * owned maybe buf; };

static char * owned keep(char * owned p) { p[0] = 'k'; return p; }
static int peek(const char * view p) { return p[0]; }

int main(void)
{
  char * owned maybe p = malloc(4);
  if (!p)
    return 1;
  p[0] = 'a';
  p[1] = 0;
  auto v = borrow(p);
  printf("borrow %c %d\n", v[0], v == p);
  char * owned q = keep(move(p));
  printf("moved %s %d\n", q, peek(q));
  struct box b = {move(q)};
  char * owned maybe r = move(b.buf);
  b.buf = 0;
  printf("member %s\n", r);
  free(r);
  return 0;
}
