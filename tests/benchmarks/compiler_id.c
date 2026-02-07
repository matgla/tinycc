/*
 * Compiler Identification
 * 
 * This file is compiled with the benchmark library (TCC or GCC)
 * to encode the compiler signature into the binary.
 */

#ifdef __TINYC__
const char *benchmark_compiler_name = "TCC";
const int benchmark_compiler_sig = 0x544343;  /* "TCC" in hex */
const char *benchmark_compiler_id = "TINYCC";
#else
const char *benchmark_compiler_name = "GCC";
const int benchmark_compiler_sig = 0x474343;  /* "GCC" in hex */
const char *benchmark_compiler_id = "GCC";
#endif
