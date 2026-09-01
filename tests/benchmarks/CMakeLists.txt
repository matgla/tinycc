cmake_minimum_required(VERSION 3.13)

# Initialize Pico SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(benchmark_picosdk C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Initialize the SDK
pico_sdk_init()

# ============================================================================
# Benchmark Library Configuration
# ============================================================================

# Select benchmark compiler: TCC or GCC
# Usage: cmake -DBENCHMARK_COMPILER=TCC .. or -DBENCHMARK_COMPILER=GCC ..
set(BENCHMARK_COMPILER "GCC" CACHE STRING "Compiler for benchmark library (TCC or GCC)")

# Select optimization level for benchmark library
# Usage: cmake -DBENCHMARK_OPT_LEVEL=0 .. or -DBENCHMARK_OPT_LEVEL=1 ..
set(BENCHMARK_OPT_LEVEL "1" CACHE STRING "Optimization level (0 or 1)")

# Benchmark source files - ONLY the actual benchmark code, compiled with selected compiler
# Timing/calibration code stays in GCC-compiled minimal_uart_picosdk.c
set(BENCHMARK_LIB_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_double.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_algorithm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/compiler_id.c
)

# MiBench adapter files (optional, enabled by default)
option(ENABLE_MIBENCH "Enable MiBench benchmarks" ON)
if(ENABLE_MIBENCH)
    message(STATUS "MiBench benchmarks: ENABLED")
    list(APPEND BENCHMARK_LIB_SRCS
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_sha.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_bitcount.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_crc32.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_dijkstra.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_qsort.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_rijndael.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_stringsearch.c
        ${CMAKE_CURRENT_SOURCE_DIR}/mibench_adapters/mibench_init.c
    )
    add_definitions(-DENABLE_MIBENCH=1)
else()
    message(STATUS "MiBench benchmarks: DISABLED")
endif()

# Common flags for benchmark library.
# NOTE: this include set is for the GCC path only (the TCC path hardcodes its
# own -I flags below). It must NOT contain tcc's own include/ directory: those
# are tcc compiler-intrinsic headers (its stddef.h deliberately omits the
# __need_wint_t -> wint_t typedef), and putting them ahead of arm-none-eabi-gcc's
# own stddef.h makes newlib's <sys/_types.h> fail with "unknown type name
# 'wint_t'". GCC uses its own toolchain headers plus the shared libc_includes.
set(BENCH_ARCH_FLAGS "-mcpu=cortex-m33" "-mthumb" "-mfloat-abi=soft")
set(BENCH_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../ir_tests/libc_includes
)

# ============================================================================
# Build Benchmark Library with selected compiler
# ============================================================================

if(BENCHMARK_COMPILER STREQUAL "TCC")
    message(STATUS "Building benchmark library with TCC -O${BENCHMARK_OPT_LEVEL}")

    # Find TCC compiler
    find_program(TCC_EXE armv8m-tcc PATHS ${CMAKE_CURRENT_SOURCE_DIR}/../..)

    if(NOT TCC_EXE)
        message(FATAL_ERROR "armv8m-tcc not found!")
    endif()

    message(STATUS "Using TCC: ${TCC_EXE}")

    # Find TCC's soft-float runtime library for fair comparison with GCC's libgcc
    # TCC_EXE is typically at <tcc_root>/armv8m-tcc, libs are at <tcc_root>/lib/fp/
    get_filename_component(TCC_DIR ${TCC_EXE} DIRECTORY)
    set(TCC_RUNTIME_LIB "${TCC_DIR}/armv8m-libtcc1.a")
    # Which FP runtime the benchmark links, and whether TCC lowers FP inline.
    # Declared here (not only in the FP-conformance section below) so that
    # `run_benchmark.py --fp-lib rp2350fp --mfpu rp2350` measures the DCP --
    # otherwise the benchmark always linked libsoftfp and a DCP-vs-soft
    # comparison silently measured the same binary twice.
    set(FP_LIB "softfp" CACHE STRING "FP runtime library from lib/fp (softfp, rp2350fp, vfpv4sp, vfpv5dp)")
    set(FP_MFPU "" CACHE STRING "Value for TCC's -mfpu= when building benchmarks")
    set(TCC_FP_LIB "${TCC_DIR}/lib/fp/lib${FP_LIB}.a")
    set(BENCH_MFPU_FLAG "")
    if(NOT FP_MFPU STREQUAL "")
        set(BENCH_MFPU_FLAG "-mfpu=${FP_MFPU}")
    endif()
    if(EXISTS ${TCC_RUNTIME_LIB})
        message(STATUS "Using TCC runtime library: ${TCC_RUNTIME_LIB}")
    else()
        message(WARNING "TCC runtime library not found at ${TCC_RUNTIME_LIB}")
        set(TCC_RUNTIME_LIB "")
    endif()
    if(EXISTS ${TCC_FP_LIB})
        message(STATUS "Using TCC soft-float runtime: ${TCC_FP_LIB}")
    else()
        message(WARNING "TCC soft-float runtime not found at ${TCC_FP_LIB}")
        set(TCC_FP_LIB "")
    endif()

    # TCC has no bundled cross libc headers, so it must be pointed at the
    # newlib headers used by the IR test suite. Without these it would fall
    # back to the host's glibc /usr/include (bits/libc-header-start.h,
    # gnu/stubs-32.h, ...) which is wrong for a cortex-m33 target and breaks
    # the build on Ubuntu/Fedora hosts alike. Order mirrors the IR harness
    # (profile_suite.py / test_gcc_torture_ir.py).
    set(TCC_LIBC_INCLUDE_FLAGS
        -I${CMAKE_CURRENT_SOURCE_DIR}/../ir_tests/libc_includes
        -I${CMAKE_CURRENT_SOURCE_DIR}/../ir_tests/libc_imports
        -I${CMAKE_CURRENT_SOURCE_DIR}/../ir_tests/libc_includes/newlib
    )

    # Define source files and their object file mappings
    set(TCC_OBJECTS "")
    set(TCC_COMPILE_COMMANDS "")

    foreach(src ${BENCHMARK_LIB_SRCS})
        get_filename_component(name ${src} NAME_WE)
        set(obj "${CMAKE_CURRENT_BINARY_DIR}/${name}_tcc.o")
        list(APPEND TCC_OBJECTS ${obj})

        # Add custom command for each object file
        add_custom_command(
            OUTPUT ${obj}
            COMMAND ${TCC_EXE} -c ${BENCH_ARCH_FLAGS} ${BENCH_MFPU_FLAG} -O${BENCHMARK_OPT_LEVEL} -g
                -DBENCHMARK_OPT_LEVEL=${BENCHMARK_OPT_LEVEL}
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
                ${TCC_LIBC_INCLUDE_FLAGS}
                ${src} -o ${obj}
            DEPENDS ${src}
            COMMENT "Compiling ${name}.c with TCC -O${BENCHMARK_OPT_LEVEL}"
        )
    endforeach()

    # Build static library from all object files
    add_custom_command(
        OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libbenchmark_tcc.a
        COMMAND arm-none-eabi-ar rcs ${CMAKE_CURRENT_BINARY_DIR}/libbenchmark_tcc.a ${TCC_OBJECTS}
        DEPENDS ${TCC_OBJECTS}
        COMMENT "Creating TCC benchmark library -O${BENCHMARK_OPT_LEVEL}..."
    )

    add_custom_target(benchmark_lib_tcc ALL
        DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libbenchmark_tcc.a
    )
    set(BENCHMARK_LIB "${CMAKE_CURRENT_BINARY_DIR}/libbenchmark_tcc.a")
    set(BENCHMARK_EXECUTABLE_SUFFIX "_tcc")

elseif(BENCHMARK_COMPILER STREQUAL "GCC")
    message(STATUS "Building benchmark library with GCC -O${BENCHMARK_OPT_LEVEL}")

    # Create static library for benchmark code
    add_library(benchmark_lib_gcc STATIC ${BENCHMARK_LIB_SRCS})

    # Set compiler flags for GCC
    target_compile_options(benchmark_lib_gcc PRIVATE
        ${BENCH_ARCH_FLAGS}
        -O${BENCHMARK_OPT_LEVEL}
        -ffunction-sections
        -fdata-sections
        -g
    )
    target_compile_definitions(benchmark_lib_gcc PRIVATE BENCHMARK_OPT_LEVEL=${BENCHMARK_OPT_LEVEL})

    target_include_directories(benchmark_lib_gcc PRIVATE ${BENCH_INCLUDE_DIRS})

    # Define GCC macro for benchmark code
    target_compile_definitions(benchmark_lib_gcc PRIVATE GCC_BENCHMARK=1)

    set(BENCHMARK_LIB benchmark_lib_gcc)
    set(BENCHMARK_EXECUTABLE_SUFFIX "_gcc")

    # ------------------------------------------------------------------
    # Soft-float runtime for the GCC arm, built from the SAME sources the
    # TCC arm uses (lib/fp/soft), so the double benchmarks compare the two
    # COMPILERS instead of comparing TCC's C against libgcc's hand-written
    # ieee754-df.S.
    #
    # Left off, the GCC image resolves every __aeabi_d* call from libgcc,
    # whose dadd/dmul/ddiv are assembly.  That made double_add read 4.12x
    # and double_mixed 3.93x while the same source compiled by both
    # compilers differs by only ~1.13x in size -- i.e. almost all of the
    # measured "gap" was implementation, not code generation.
    #
    # libsoftfp defines the complete __aeabi_ set on its own (lib/fp's
    # check-self-contained target enforces exactly this), so listing it
    # ahead of libgcc resolves every FP entry point here and libgcc is
    # never pulled in for them -- no duplicate-definition link errors.
    #
    # Set BENCH_GCC_FP_FROM_SOURCE=OFF to restore the libgcc baseline.
    option(BENCH_GCC_FP_FROM_SOURCE
           "Build GCC's soft-float runtime from lib/fp/soft instead of using libgcc" ON)

    if(BENCH_GCC_FP_FROM_SOURCE)
        set(FP_SOFT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/fp/soft)
        # Mirrors SRCS in lib/fp/soft/Makefile -- keep in step with it.
        set(FP_SOFT_SRCS
            fadd.c fmul.c fdiv.c fcmp.c dadd.c dmul.c ddiv.c
            dconv.c dcmp.c conv.c conv64.c
            f2d_stub.S fcmp_asm.S dcmp_asm.S)
        set(FP_SOFT_PATHS "")
        foreach(f ${FP_SOFT_SRCS})
            list(APPEND FP_SOFT_PATHS ${FP_SOFT_DIR}/${f})
        endforeach()

        add_library(softfp_gcc STATIC ${FP_SOFT_PATHS})
        # Same flags lib/fp/soft/Makefile uses (-O2 regardless of the
        # benchmark's opt level: the TCC arm links a prebuilt libsoftfp.a
        # that is always built -O2, so matching it keeps the arms honest).
        target_compile_options(softfp_gcc PRIVATE
            ${BENCH_ARCH_FLAGS} -O2 -Wall -Wextra -g)
        target_include_directories(softfp_gcc PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../lib
            ${CMAKE_CURRENT_SOURCE_DIR}/../../lib/fp
            ${CMAKE_CURRENT_SOURCE_DIR}/../../include)
        set(GCC_FP_LIB softfp_gcc)
        message(STATUS "GCC soft-float runtime built from lib/fp/soft (same sources as TCC)")
    else()
        set(GCC_FP_LIB "")
        message(STATUS "GCC soft-float runtime: libgcc (NOT source-matched with TCC)")
    endif()

else()
    message(FATAL_ERROR "BENCHMARK_COMPILER must be TCC or GCC")
endif()

# ============================================================================
# Pico SDK Executable (links benchmark library)
# ============================================================================

add_executable(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
    minimal_uart_picosdk.c
    benchmark_main.c
    cycle_counter.c
    hardfault_handler.c
)

# For the TCC path the benchmark library is produced by a custom command and
# linked by file path, so CMake does not infer a target dependency on it.
# Make it explicit, otherwise a parallel `make -j` (as run_benchmark.py uses)
# can start the executable link before libbenchmark_tcc.a exists.
if(BENCHMARK_COMPILER STREQUAL "TCC")
    add_dependencies(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX} benchmark_lib_tcc)
endif()

# Link to Pico SDK libraries
# Note: We intentionally don't use pico_float here to compare compiler's built-in soft-float
target_link_libraries(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
    pico_stdlib
    hardware_uart
    hardware_gpio
    hardware_timer
    ${BENCHMARK_LIB}
)

# Use compiler's own soft-float implementation for fair TCC vs GCC comparison
# (instead of Pico SDK's optimized pico_float library)
pico_set_float_implementation(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX} compiler)

# ...and the same for DOUBLE, which is not implied by the float setting.  Left
# unset, pico_double_default wins and the SDK links its own DCP assembly with
# `-Wl,--wrap=__aeabi_dadd` (and friends), so every double operation is
# intercepted before it can reach the compiler's runtime.  The symptom is
# subtle: `--fp-lib softfp` and `--fp-lib rp2350fp` produce byte-identical
# images, because neither one's FP library is ever called.  Any TCC-vs-GCC
# double comparison made before this line was also measuring pico-sdk, not the
# compilers.
pico_set_double_implementation(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX} compiler)

# For TCC builds, also link TCC's runtime libraries explicitly.
# TCC-generated objects can reference helper symbols from armv8m-libtcc1.a
# and software floating-point entry points from libsoftfp.a.
if(BENCHMARK_COMPILER STREQUAL "TCC" AND (TCC_RUNTIME_LIB OR TCC_FP_LIB))
    target_link_libraries(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
        -Wl,--start-group ${TCC_RUNTIME_LIB} ${TCC_FP_LIB} -Wl,--end-group
    )
    message(STATUS "Linked TCC runtime libraries for benchmark build")
endif()

# The GCC arm's source-matched soft-float runtime (see BENCH_GCC_FP_FROM_SOURCE).
# It must precede libgcc on the link line, which target_link_libraries does:
# libgcc is appended by the compiler driver after every explicit library.
if(BENCHMARK_COMPILER STREQUAL "GCC" AND GCC_FP_LIB)
    target_link_libraries(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
        ${GCC_FP_LIB}
    )
    message(STATUS "Linked source-matched soft-float runtime for GCC benchmark build")
endif()

target_compile_definitions(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
    PUBLIC
    PICO_DEFAULT_UART_TX_PIN=32
    PICO_DEFAULT_UART_RX_PIN=33
)

# Create map/bin/hex/uf2 files
pico_add_extra_outputs(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX})

# Set linker script for RAM execution (no flash)
pico_set_binary_type(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX} copy_to_ram)

# ============================================================================
# FP conformance executable (separate image; see fp_conformance_main.c for why)
#
# Configure with -DBUILD_FP_CONFORMANCE=ON.  The FP runtime under test is
# selected by -DFP_LIB=<name>, matching the short library names in lib/fp:
#   softfp    (default)  pure software floating point
#   rp2350fp             RP2350 double coprocessor
# and the compiler's own lowering is selected by -DFP_MFPU=<name>, e.g.
#   -DFP_MFPU=rp2350 to emit inline DCP/VFP instead of __aeabi_* calls.
# ============================================================================

option(BUILD_FP_CONFORMANCE "Build the floating-point conformance image" OFF)
# FP_LIB / FP_MFPU are declared in the TCC block above (shared with the
# benchmark image); the conformance image reuses them as-is.

if(BUILD_FP_CONFORMANCE)
    set(FP_CONF_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../fp)
    set(FP_CONF_SRC ${FP_CONF_DIR}/fp_conformance.c)

    if(BENCHMARK_COMPILER STREQUAL "TCC")
        set(FP_TCC_LIB "${TCC_DIR}/lib/fp/lib${FP_LIB}.a")
        if(NOT EXISTS ${FP_TCC_LIB})
            message(FATAL_ERROR "FP library not found: ${FP_TCC_LIB} (build it with `make fp-libs`)")
        endif()

        set(FP_MFPU_FLAG "")
        if(NOT FP_MFPU STREQUAL "")
            set(FP_MFPU_FLAG "-mfpu=${FP_MFPU}")
        endif()

        set(FP_CONF_OBJ "${CMAKE_CURRENT_BINARY_DIR}/fp_conformance_tcc.o")
        add_custom_command(
            OUTPUT ${FP_CONF_OBJ}
            COMMAND ${TCC_EXE} -c ${BENCH_ARCH_FLAGS} ${FP_MFPU_FLAG} -O${BENCHMARK_OPT_LEVEL} -g
                -I${FP_CONF_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
                ${TCC_LIBC_INCLUDE_FLAGS}
                ${FP_CONF_SRC} -o ${FP_CONF_OBJ}
            DEPENDS ${FP_CONF_SRC} ${FP_CONF_DIR}/fp_vectors.h ${FP_CONF_DIR}/fp_conformance.h
            COMMENT "Compiling fp_conformance.c with TCC -O${BENCHMARK_OPT_LEVEL} ${FP_MFPU_FLAG}"
        )
        add_custom_target(fp_conformance_obj ALL DEPENDS ${FP_CONF_OBJ})

        add_executable(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} fp_conformance_main.c)
        add_dependencies(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} fp_conformance_obj)
        target_link_libraries(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX}
            ${FP_CONF_OBJ}
            -Wl,--start-group ${TCC_RUNTIME_LIB} ${FP_TCC_LIB} -Wl,--end-group
        )
        set(FP_BUILD_ID "tcc-O${BENCHMARK_OPT_LEVEL}-${FP_LIB}${FP_MFPU_FLAG}")
    else()
        add_executable(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} fp_conformance_main.c ${FP_CONF_SRC})
        target_compile_options(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} PRIVATE
            ${BENCH_ARCH_FLAGS} -O${BENCHMARK_OPT_LEVEL} -g)
        set(FP_BUILD_ID "gcc-O${BENCHMARK_OPT_LEVEL}-libgcc")
    endif()

    target_include_directories(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} PRIVATE
        ${FP_CONF_DIR} ${BENCH_INCLUDE_DIRS})

    target_compile_definitions(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} PUBLIC
        PICO_DEFAULT_UART_TX_PIN=32
        PICO_DEFAULT_UART_RX_PIN=33
        FP_CONFORMANCE_BUILD_ID="${FP_BUILD_ID}")

    target_link_libraries(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX}
        pico_stdlib hardware_uart hardware_gpio)

    # Same reasoning as the benchmark image: use the compiler's own FP lowering
    # rather than the SDK's optimized pico_float, since that lowering is what
    # is under test.
    pico_set_float_implementation(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} compiler)
    pico_set_double_implementation(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} compiler)

    pico_add_extra_outputs(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX})
    pico_set_binary_type(fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} copy_to_ram)

    message(STATUS "FP conformance image: fp_conformance${BENCHMARK_EXECUTABLE_SUFFIX} (${FP_BUILD_ID})")
endif()

# ============================================================================
# Instruction-width demo (separate image; see width_demo_main.c for why)
#
# Configure with -DBUILD_WIDTH_DEMO=ON.  Always GCC-built and always
# compiler-independent: the units under test are the hand-written Thumb-2
# sequences in width_kernels.S, so BENCHMARK_COMPILER has no bearing on what
# is measured and the image is not part of the tracked benchmark numbers.
# ============================================================================

option(BUILD_WIDTH_DEMO "Build the instruction-width vs cycles demo image" OFF)

if(BUILD_WIDTH_DEMO)
    add_executable(width_demo
        ${CMAKE_CURRENT_SOURCE_DIR}/width_demo_main.c
        ${CMAKE_CURRENT_SOURCE_DIR}/width_kernels.S
    )

    target_compile_options(width_demo PRIVATE ${BENCH_ARCH_FLAGS} -O2 -g)

    # Deliberately NOT ${BENCH_INCLUDE_DIRS}: that set puts the IR suite's
    # libc_includes ahead of the toolchain's own headers, and the SDK's
    # pico_float / pico_double sources then compile against a math.h with no
    # HUGE_VALF and no log2/exp2/ldexp/remquo.  This image needs nothing from
    # there -- its only headers are the SDK's and the C library's.

    target_compile_definitions(width_demo PUBLIC
        PICO_DEFAULT_UART_TX_PIN=32
        PICO_DEFAULT_UART_RX_PIN=33)

    target_link_libraries(width_demo pico_stdlib hardware_uart hardware_gpio hardware_sync)

    # Nothing here is floating point; keep the SDK's optimized FP libraries out
    # of the image so it stays small and quick to rebuild.
    pico_set_float_implementation(width_demo compiler)
    pico_set_double_implementation(width_demo compiler)

    pico_add_extra_outputs(width_demo)

    # Run from SRAM: the point of the measurement is the core's cost per
    # instruction, and executing from XIP flash would fold the cache into
    # every number.
    pico_set_binary_type(width_demo copy_to_ram)

    message(STATUS "Instruction-width demo image: width_demo")
endif()

message(STATUS "")
message(STATUS "==========================================")
message(STATUS "Benchmark Configuration:")
message(STATUS "  Compiler: ${BENCHMARK_COMPILER}")
message(STATUS "  Optimization: -O${BENCHMARK_OPT_LEVEL}")
message(STATUS "  Executable: minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}")
message(STATUS "==========================================")
message(STATUS "")
