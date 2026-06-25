/*
 * MiBench Benchmark Suite Initialization
 *
 * Registers all MiBench benchmarks with the RP2350 benchmark harness.
 */

#include "../benchmarks.h"

/* External init functions for each benchmark */
extern void init_mibench_sha(void);
extern void init_mibench_bitcount(void);
extern void init_mibench_crc32(void);
extern void init_mibench_dijkstra(void);
extern void init_mibench_qsort(void);
extern void init_mibench_rijndael(void);
extern void init_mibench_stringsearch(void);

/* Main initialization - call from benchmark_main.c */
void init_mibench_benchmarks(void)
{
    /* Phase 1: Easy, self-contained benchmarks */
    init_mibench_sha();
    init_mibench_bitcount();
    init_mibench_crc32();
    init_mibench_dijkstra();
    init_mibench_qsort();
    init_mibench_rijndael();
    init_mibench_stringsearch();

    /* TODO: Phase 1 additions
    init_mibench_patricia();
    init_mibench_blowfish();
    init_mibench_fft();
    init_mibench_adpcm();
    init_mibench_gsm();
    */

    /* TODO: Phase 2
    init_mibench_mad();
    init_mibench_ispell();
    init_mibench_rsynth();
    init_mibench_basicmath();
    */

    /* TODO: Phase 3
    init_mibench_susan();
    init_mibench_jpeg();
    init_mibench_lame();
    init_mibench_blowfish();
    */
}
