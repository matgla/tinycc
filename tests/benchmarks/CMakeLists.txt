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
    set(TCC_FP_LIB "${TCC_DIR}/lib/fp/libsoftfp.a")
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
            COMMAND ${TCC_EXE} -c ${BENCH_ARCH_FLAGS} -O${BENCHMARK_OPT_LEVEL} -g
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

# For TCC builds, also link TCC's runtime libraries explicitly.
# TCC-generated objects can reference helper symbols from armv8m-libtcc1.a
# and software floating-point entry points from libsoftfp.a.
if(BENCHMARK_COMPILER STREQUAL "TCC" AND (TCC_RUNTIME_LIB OR TCC_FP_LIB))
    target_link_libraries(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
        -Wl,--start-group ${TCC_RUNTIME_LIB} ${TCC_FP_LIB} -Wl,--end-group
    )
    message(STATUS "Linked TCC runtime libraries for benchmark build")
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

message(STATUS "")
message(STATUS "==========================================")
message(STATUS "Benchmark Configuration:")
message(STATUS "  Compiler: ${BENCHMARK_COMPILER}")
message(STATUS "  Optimization: -O${BENCHMARK_OPT_LEVEL}")
message(STATUS "  Executable: minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}")
message(STATUS "==========================================")
message(STATUS "")
