/* dead_local_slot: a memcpy source tagged "tame" must have its read recorded,
   or the stores that fill it look dead.  The pointer may be one of several
   locals (`p = tag ? &a.cfg : &b.cfg`, here a Zig tagged union copied out by
   value, as yasos' SD driver does in Mmc.get_config -- the card never
   initialised), or point inside one (`&x + 16`). */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint32_t clk, cmd, d0; } Pins;
typedef struct { uint32_t clock_speed, timeout_ms; Pins pins; uint8_t bus_width; bool use_dma; uint8_t mode; } Cfg;
typedef struct { Cfg _config; uint32_t miso, mosi, sclk, cs, sm; void *pio; } Spi;
typedef struct { uint64_t a, b; Cfg _config; bool init; uint8_t tp, c1, c2, c3, c4, c5, c6; } Sdio;
typedef struct { union { Spi spi; Sdio sdio; } payload; uint8_t tag; } Mmc;
typedef struct { Mmc impl; } MmcW;

static Cfg inner(const Mmc a0)
{
  Sdio t7, t8; Cfg t1, t6; Spi t2, t3; const Spi *t4; const Cfg *t5; const Sdio *t9;
  uint8_t t0 = a0.tag;
  switch (t0) {
  case 0: { t2 = a0.payload.spi; t3 = t2; t4 = &t3; t5 = &t4->_config; t6 = *t5; t1 = t6; goto blk; }
  case 1: { t7 = a0.payload.sdio; t8 = t7; t9 = &t8; t5 = &t9->_config; t6 = *t5; t1 = t6; goto blk; }
  default: __builtin_unreachable();
  }
blk:;
  return t1;
}
__attribute__((noinline)) Cfg get_config(const MmcW *const a0)
{
  Mmc t4; const MmcW *const *t1; const MmcW *t2; const MmcW *t0; const Mmc *t3; Cfg t5;
  t0 = a0; t1 = &t0; t2 = *t1; t3 = &t2->impl; t4 = *t3; t5 = inner(t4); return t5;
}
/* one slot, a pointer into its middle that reaches the copy through a VAR */
typedef struct { int a[4]; Cfg c; int z[2]; } Big;
__attribute__((noinline)) Cfg interior(int k)
{
  Big x;
  const Cfg *p;
  for (int i = 0; i < 4; i++) x.a[i] = k + i;
  x.c = (Cfg){ (uint32_t)k * 10u, (uint32_t)k * 11u, { 1, 2, (uint32_t)k }, 3, 1, 5 };
  x.z[0] = x.z[1] = k;
  p = &x.c;
  if (k > 100)
    p = (const Cfg *)&x.a[1];
  Cfg out;
  __builtin_memcpy(&out, p, sizeof out);
  return out;
}
int main(void)
{
  MmcW m = {0};
  m.impl.tag = 1;
  m.impl.payload.sdio.a = 0x1111; m.impl.payload.sdio.b = 0x2222;
  m.impl.payload.sdio._config = (Cfg){ 50000000u, 1000u, { 7, 8, 9 }, 4, 1, 2 };
  Cfg c = get_config(&m);
  printf("sdio %u %u %u %u %u %u %u %u\n", c.clock_speed, c.timeout_ms, c.pins.clk, c.pins.cmd, c.pins.d0, c.bus_width, c.use_dma, c.mode);
  MmcW s = {0};
  s.impl.tag = 0;
  s.impl.payload.spi._config = (Cfg){ 400000u, 250u, { 1, 2, 3 }, 1, 0, 0 };
  s.impl.payload.spi.miso = 99;
  c = get_config(&s);
  printf("spi %u %u %u %u %u %u %u %u\n", c.clock_speed, c.timeout_ms, c.pins.clk, c.pins.cmd, c.pins.d0, c.bus_width, c.use_dma, c.mode);
  c = interior(7);
  printf("interior %u %u %u %u %u %u %u %u\n", c.clock_speed, c.timeout_ms, c.pins.clk, c.pins.cmd, c.pins.d0, c.bus_width, c.use_dma, c.mode);
  return 0;
}
