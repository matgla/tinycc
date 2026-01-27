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

# Benchmark source files (all except platform-specific main files)
set(BENCHMARK_LIB_SRCS
    ${CMAKE_CURRENT_SOURCE_DIR}/benchmark_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_math.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_control.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_string.c
    ${CMAKE_CURRENT_SOURCE_DIR}/bench_algorithm.c
    ${CMAKE_CURRENT_SOURCE_DIR}/cycle_counter.c
)

# Common flags for benchmark library
set(BENCH_ARCH_FLAGS "-mcpu=cortex-m33" "-mthumb" "-mfloat-abi=soft")
set(BENCH_INCLUDE_DIRS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../include
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
                -I${CMAKE_CURRENT_SOURCE_DIR}
                -I${CMAKE_CURRENT_SOURCE_DIR}/../ir_tests/libc_includes
                -I${CMAKE_CURRENT_SOURCE_DIR}/../../include
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
)

# Link to Pico SDK libraries
target_link_libraries(minimal_uart_picosdk${BENCHMARK_EXECUTABLE_SUFFIX}
    pico_stdlib
    hardware_uart
    hardware_gpio
    ${BENCHMARK_LIB}
)

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
