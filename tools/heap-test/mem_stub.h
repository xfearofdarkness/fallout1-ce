// Minimal stand-ins for plib/gnw/memory.cc and debug.cc so that
// src/game/heap.cc can be built and tested on its own.
//
// mem_malloc mimics the real block layout on 64-bit targets:
// 16-byte header {size_t size; int guard (0xFEEDFACE); int padding} before
// the returned pointer. This matters because an out-of-range read of
// `heap_free_list[-1]` then yields exactly 0xFEEDFACE, as in real crashes.

#ifndef FALLOUT_TOOLS_HEAP_TEST_MEM_STUB_H_
#define FALLOUT_TOOLS_HEAP_TEST_MEM_STUB_H_

#include <cstdarg>
#include <cstdio>
#include <cstdlib>

namespace fallout {

void* mem_malloc(size_t size)
{
    unsigned char* block = (unsigned char*)calloc(1, size + 24);
    *(size_t*)block = size;
    *(unsigned int*)(block + 8) = 0xFEEDFACE;
    *(unsigned int*)(block + 12) = 0;
    return block + 16;
}

void* mem_realloc(void* ptr, size_t size)
{
    if (ptr == NULL) {
        return mem_malloc(size);
    }

    unsigned char* block = (unsigned char*)realloc((unsigned char*)ptr - 16, size + 24);
    *(size_t*)block = size;
    return block + 16;
}

void mem_free(void* ptr)
{
    if (ptr != NULL) {
        free((unsigned char*)ptr - 16);
    }
}

int debug_printf(const char* format, ...)
{
    if (getenv("HEAPDBG") != NULL) {
        va_list args;
        va_start(args, format);
        vfprintf(stderr, format, args);
        va_end(args);
    }
    return 0;
}

} // namespace fallout

#endif /* FALLOUT_TOOLS_HEAP_TEST_MEM_STUB_H_ */
