# Standalone tests for src/game/heap.cc (no SDL, no game data needed).
#
#   cmake -S tools/heap-test -B build-heap-test
#   cmake --build build-heap-test
#   ./build-heap-test/heap_fuzz 1 20000
#   ./build-heap-test/heap_overrun 12

cmake_minimum_required(VERSION 3.13)
project(heap-test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED YES)

option(ASAN "Enable address sanitizer" OFF)

foreach(target heap_fuzz heap_overrun)
    add_executable(${target} ${target}.cc ../../src/game/heap.cc)
    target_include_directories(${target} PRIVATE ../../src)
    if(ASAN)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined)
        target_link_options(${target} PRIVATE -fsanitize=address,undefined)
    endif()
endforeach()
