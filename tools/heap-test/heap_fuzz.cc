// Randomized test for src/game/heap.cc: allocate/lock/unlock/free blocks
// (including compaction of moveable blocks) and validate heap metadata and
// block contents after every operation.
//
// Usage: heap_fuzz [seed] [iterations]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "game/heap.h"
#include "mem_stub.h"

using namespace fallout;

struct Block {
    int handle;
    int size;
    bool locked;
    unsigned char tag;
};

int main(int argc, char** argv)
{
    unsigned int seed = argc > 1 ? atoi(argv[1]) : 1;
    int iterations = argc > 2 ? atoi(argv[2]) : 20000;
    srand(seed);

    Heap heap;
    if (!heap_init(&heap, 64 * 1024)) {
        return 2;
    }

    std::vector<Block> blocks;
    for (int it = 0; it < iterations; it++) {
        int op = rand() % 10;
        if (op < 4) {
            Block block;
            block.size = 1 + rand() % (rand() % 4 == 0 ? 8000 : 600);
            block.locked = false;
            block.tag = rand();
            // cache_add() uses 1 (no system memory fallback) first.
            if (heap_allocate(&heap, &(block.handle), block.size, rand() % 2)) {
                unsigned char* data;
                if (!heap_lock(&heap, block.handle, &data)) {
                    printf("lock after allocate failed, seed=%u it=%d\n", seed, it);
                    return 1;
                }
                memset(data, block.tag, block.size);
                if (rand() % 2) {
                    heap_unlock(&heap, block.handle);
                } else {
                    block.locked = true;
                }
                blocks.push_back(block);
            }
        } else if (op < 6 && !blocks.empty()) {
            int index = rand() % blocks.size();
            Block& block = blocks[index];
            if (block.locked) {
                heap_unlock(&heap, block.handle);
            }
            if (!heap_deallocate(&heap, &(block.handle))) {
                printf("deallocate failed, seed=%u it=%d\n", seed, it);
                return 1;
            }
            blocks.erase(blocks.begin() + index);
        } else if (op < 9 && !blocks.empty()) {
            Block& block = blocks[rand() % blocks.size()];
            if (block.locked) {
                heap_unlock(&heap, block.handle);
                block.locked = false;
            } else {
                unsigned char* data;
                if (!heap_lock(&heap, block.handle, &data)) {
                    printf("lock failed, seed=%u it=%d\n", seed, it);
                    return 1;
                }
                for (int i = 0; i < block.size; i++) {
                    if (data[i] != block.tag) {
                        printf("data corrupted, seed=%u it=%d\n", seed, it);
                        return 1;
                    }
                }
                block.locked = true;
            }
        }

        if (!heap_validate(&heap) || !heap_check(&heap)) {
            printf("heap validation failed, seed=%u it=%d\n", seed, it);
            return 1;
        }
    }

    printf("ok seed=%u\n", seed);
    return 0;
}
