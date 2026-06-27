/* Sections with explicit alignment and ordering checks. */
__attribute__((section(".custom_text"))) int custom_fn(void) { return 1; }

int regular_fn(void) { return 2; }

__attribute__((aligned(16))) int aligned_var = 0xAA;
int regular_var = 0xBB;
