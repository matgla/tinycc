/* With -fdata-sections each global would traditionally get its own
 * .data.<name>/.bss.<name> subsection so an unused one can be garbage
 * collected by the linker. */
int data_a = 1;
int data_b = 2;
