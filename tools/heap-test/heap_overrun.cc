// Reproduces the "EXC_BAD_ACCESS at 0xfeedfad2 in heap_allocate" crash
// signature: a write past the end of a locked heap block followed by an
// allocation.
//
// Usage: heap_overrun <overrun-bytes> [fill-byte]
//
// With 12 or more bytes of zeroes heap_find_free_block() reads
// heap_free_list[-1] (the mem_malloc guard 0xFEEDFACE) and dereferences
// ->size at +4, i.e. 0xFEEDFAD2. heap_check()/heap_check_block() detect
// even a 1-byte overrun.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "game/heap.h"
#include "mem_stub.h"

using namespace fallout;

int main(int argc, char** argv)
{
    int overrun = argc > 1 ? atoi(argv[1]) : 12;
    int fill = argc > 2 ? atoi(argv[2]) : 0;

    Heap heap;
    heap_init(&heap, 64 * 1024);

    int a;
    int b;
    int c;
    int d;
    heap_allocate(&heap, &a, 1000, 1);
    heap_allocate(&heap, &b, 1000, 1);
    heap_allocate(&heap, &c, 1000, 1);
    heap_allocate(&heap, &d, 1000, 1);
    heap_deallocate(&heap, &c);

    unsigned char* data;
    heap_lock(&heap, b, &data);

    // The buggy writer: 1004 bytes are usable (size rounded up to 4).
    memset(data + 1004, fill, overrun);

    printf("heap_check: %d, heap_check_block: %d\n", heap_check(&heap), heap_check_block(&heap, b));
    fflush(stdout);

    heap_unlock(&heap, b);

    int e;
    printf("allocating...\n");
    fflush(stdout);
    bool rc = heap_allocate(&heap, &e, 2000, 1);
    printf("allocated: %d\n", rc);
    return 0;
}
