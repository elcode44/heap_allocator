#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/*
 * FRAGMENTED-HEAP BENCHMARK (single thread)
 * -----------------------------------------
 * Builds a heap with thousands of free "holes" of mixed sizes, then runs a long
 * stream of mallocs and frees on top of it. With ONE free list, every malloc has
 * to walk past hundreds of too-small holes. With size bins it jumps to the right one.
 *
 * Build it against whichever allocator you want to measure:
 *   gcc -O2 -pthread -o bench bench_freelist.c malloc.c       (our allocator)
 *   gcc -O2 -DUSE_GLIBC -o bench_glibc bench_freelist.c       (the real malloc)
 */
#ifdef USE_GLIBC
#define ALLOC(n) malloc(n)
#define FREE(p)  free(p)
#define INIT()   ((void)0)
#else
#include "include/malloc.h"
#define ALLOC(n) my_malloc(n)
#define FREE(p)  my_free(p)
#define INIT()   my_heap_init()
#endif

#define SLOTS 40000
#define OPS   400000

static void* ptrs[SLOTS];

int main(void){
    INIT();
    srand(42);

    //fill every slot with a block of a random size from 16 to 2000 bytes
    for(int i = 0; i < SLOTS; i++){
        ptrs[i] = ALLOC((size_t)(rand() % 2000 + 16));
    }
    //free every other one: ~20000 holes, none touching, all different sizes
    for(int i = 0; i < SLOTS; i += 2){
        FREE(ptrs[i]);
        ptrs[i] = NULL;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    //random malloc/free churn on top of the fragmented heap
    for(int i = 0; i < OPS; i++){
        int slot = rand() % SLOTS;
        if(ptrs[slot] != NULL){
            FREE(ptrs[slot]);
            ptrs[slot] = NULL;
        }
        else{
            ptrs[slot] = ALLOC((size_t)(rand() % 2000 + 16));
        }
    }

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("%d ops on a fragmented heap: %.1f ms  (%.2f M ops/sec)\n", OPS, ms, OPS / ms / 1e3);
    return 0;
}
