/* An image laid out like a partitioned RP2350 flash (yasos.zig's
 * hal/source/raspberry/rp2350/linker_script.ld): code and data run at the
 * partition's mapped address but load 0x20000 higher, and a data-only block
 * is loaded at flash offset 0 while linked at the untranslated alias.  The
 * .ld files next to this place it three ways; test_linker.py checks VMA,
 * LMA and the LOADADDR() symbols.
 */

/* The partition table stand-in: 16 bytes, its own section. */
__attribute__((section(".partition_table"), used))
const unsigned pt_words[4] = {0xffffded3u, 0x11111111u, 0x22222222u, 0xab123579u};

int pt_counter = 42;

/* The script symbols, referenced so the link has to resolve them. */
extern char data_load[], data_load_minus[], data_load_plus[], data_load_rhs[];

unsigned pt_symbols(void)
{
  return (unsigned)data_load ^ (unsigned)data_load_minus ^ (unsigned)data_load_plus ^ (unsigned)data_load_rhs;
}

int _start(void)
{
  return pt_counter + (int)pt_symbols();
}
