/* Phase 4: load/store/lea with addressing modes. */

int load_global(void);
void store_global(int);

static int g;

int load_global(void) { return g; }
void store_global(int v) { g = v; }

int load_array(int *p, int i) { return p[i]; }
void store_array(int *p, int i, int v) { p[i] = v; }

int load_struct(int *p) { return p[3]; }
void store_struct(int *p, int v) { p[3] = v; }

int *lea_local(int *p) { int x; return &x; }
