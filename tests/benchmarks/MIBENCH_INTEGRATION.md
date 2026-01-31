# MiBench Integration Plan for RP2350 Benchmark Suite

## Overview

Integrate [MiBench](https://github.com/embecosm/mibench) embedded benchmark suite
to provide comprehensive real-world benchmarks for TCC vs GCC comparison on RP2350.

## Current State

- 12 micro-benchmarks (math, string, control flow)
- ~100-1000 cycles per iteration
- Simple verification (expected results)
- Total runtime: ~1 second per compiler

## MiBench Categories

### 1. Automotive (4 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| basicmath | Basic math operations | Small | Small | ✅ Yes |
| bitcount | Bit manipulation | Small | Small | ✅ Yes |
| qsort | Quick sort algorithm | Small | Small | ✅ Yes |
| susan | Image recognition | Medium | Large (256KB+) | ⚠️ Maybe |

### 2. Consumer (7 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| jpeg | JPEG encode/decode | Large | Large | ⚠️ Check |
| lame | MP3 encoder | Large | Medium | ⚠️ Check |
| mad | MP3 decoder | Medium | Medium | ✅ Yes |
| tiff* | TIFF processing | Large | Large | ❌ No (complex build) |
| typeset | Text typesetting | Medium | Medium | ✅ Yes |

### 3. Network (2 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| dijkstra | Shortest path | Small | Medium | ✅ Yes |
| patricia | Patricia trie | Small | Medium | ✅ Yes |

### 4. Office (5 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| ghostscript | PostScript | Very Large | Large | ❌ No |
| ispell | Spell checker | Medium | Medium | ✅ Yes |
| rsynth | Speech synthesis | Medium | Medium | ✅ Yes |
| sphinx | Speech recognition | Large | Large | ⚠️ Check |
| stringsearch | String search | Small | Small | ✅ Yes |

### 5. Security (4 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| blowfish | Encryption | Small | Small | ✅ Yes |
| pgp | Encryption suite | Large | Large | ❌ No |
| rijndael | AES encryption | Small | Small | ✅ Yes |
| sha | SHA hashing | Small | Small | ✅ Yes |

### 6. Telecomm (4 benchmarks)
| Benchmark | Description | Code Size | Data Size | Suitable? |
|-----------|-------------|-----------|-----------|-----------|
| CRC32 | Checksum | Small | Small | ✅ Yes |
| FFT | Fast Fourier Transform | Small | Small | ✅ Yes |
| adpcm | Audio compression | Small | Small | ✅ Yes |
| gsm | Speech compression | Medium | Medium | ✅ Yes |

## Selected Subset for Initial Integration

**Phase 1 - Easy wins (13 benchmarks):**
- basicmath, bitcount, qsort
- dijkstra, patricia
- blowfish, rijndael, sha
- CRC32, FFT, adpcm, gsm
- stringsearch

**Phase 2 - Medium complexity (4 benchmarks):**
- mad (MP3 decoder)
- ispell, rsynth
- typeset

**Phase 3 - Complex (4 benchmarks):**
- susan, jpeg, lame, sphinx

## Integration Architecture

```
tests/benchmarks/
├── bench_*.c                 # Existing micro-benchmarks
├── mibench/
│   ├── sources/              # Cloned MiBench sources
│   │   ├── automotive/
│   │   ├── network/
│   │   ├── security/
│   │   ├── telecomm/
│   │   └── ...
│   ├── adapters/             # Adapter files
│   │   ├── mibench_basicmath.c
│   │   ├── mibench_bitcount.c
│   │   └── ...
│   ├── data/                 # Test data files (inputs)
│   │   ├── small/            # Small dataset (fast test)
│   │   └── large/            # Large dataset (comprehensive)
│   └── CMakeLists.txt        # MiBench-specific build
├── benchmark_main.c          # Modified to init MiBench
└── run_benchmark.py          # Updated to run MiBench subset
```

## Adapter Pattern

Each MiBench benchmark needs an adapter to integrate with our harness:

```c
// mibench/adapters/mibench_basicmath.c
#include "benchmarks.h"
#include "../sources/automotive/basicmath/basicmath_small.c"

int bench_mibench_basicmath(int iterations) {
    for (int i = 0; i < iterations; i++) {
        // Run basicmath with small dataset
        basicmath_run_small();
    }
    return 0; // verification done internally
}

void init_mibench_benchmarks(void) {
    register_benchmark_ex("mibench_basicmath", bench_mibench_basicmath, 
                          100, "MiBench: Basic math", 0);
    // ... more
}
```

## Build System Changes

### CMakeLists.txt additions:

```cmake
# MiBench integration
option(ENABLE_MIBENCH "Enable MiBench benchmarks" ON)

if(ENABLE_MIBENCH)
    # Clone MiBench if not present
    if(NOT EXISTS ${CMAKE_SOURCE_DIR}/mibench/sources)
        execute_process(
            COMMAND git clone https://github.com/embecosm/mibench.git 
                    ${CMAKE_SOURCE_DIR}/mibench/sources
        )
    endif()
    
    # Add MiBench sources to build
    file(GLOB MIBENCH_ADAPTERS mibench/adapters/*.c)
    target_sources(benchmark PRIVATE ${MIBENCH_ADAPTERS})
    
    # Include paths
    target_include_directories(benchmark PRIVATE 
        mibench/sources/automotive/basicmath
        mibench/sources/automotive/bitcount
        ...
    )
endif()
```

## Test Data Management

MiBench requires input data files. Options:

1. **Generate synthetic data** at runtime (preferred for embedded)
2. **Embed small data files** in flash (use objcopy)
3. **Use existing small datasets** from MiBench

Example for synthetic data:
```c
void generate_input_data(void) {
    // Generate deterministic test data
    for (int i = 0; i < DATA_SIZE; i++) {
        input_buffer[i] = (i * 7 + 13) & 0xFF;
    }
}
```

## Expected Results Verification

Many MiBench benchmarks don't have deterministic outputs (e.g., image processing).
Options:

1. **Skip verification** for output-variable benchmarks (measure only performance)
2. **CRC check** output against known-good value
3. **Golden reference** comparison (store expected output)

## Script Updates

run_benchmark.py changes:
- Add `--mibench` flag to run only MiBench
- Add `--mibench-subset={phase1,phase2,phase3,all}`
- Separate output tables for micro vs MiBench benchmarks
- Timeout handling (some MiBench tests run longer)

## Memory Budget

RP2350 has:
- 520 KB SRAM
- 4 MB external flash (XIP)

Budget per benchmark:
- Code: < 100 KB
- Data: < 200 KB
- Stack: < 50 KB

## Timeline Estimate

| Phase | Benchmarks | Effort | Status |
|-------|-----------|--------|--------|
| 1 | 13 easy | 2-3 days | Planned |
| 2 | 4 medium | 1-2 days | Planned |
| 3 | 4 complex | 2-3 days | Planned |

Total: ~1 week for full integration

## Next Steps

1. Clone MiBench repository
2. Create adapter template
3. Implement 3 pilot benchmarks (basicmath, sha, fft)
4. Test on RP2350
5. Expand to full Phase 1
