#include <stdio.h>
#include <stdlib.h>
#include "include/malloc.h"

/*
 * MEMORY OVERHEAD BENCHMARK
 * -------------------------
 * For several request-size patterns, allocate 20,000 blocks, then compare
 *      bytes the program asked for   vs   bytes the allocator actually used
 * (used = payload + header + footer + rounding up for alignment).
 *
 * utilization = requested / used.  100% would mean zero overhead.
 * Build it with different alignments to see what alignment costs:
 *      gcc -O2 -pthread -DMY_ALLOC_ALIGNMENT=16 -o bench_memory_16 bench_memory.c malloc.c
 *      gcc -O2 -pthread -DMY_ALLOC_ALIGNMENT=64 -o bench_memory_64 bench_memory.c malloc.c
 */
#define N 20000

typedef struct { const char* name; size_t lo; size_t hi; } pattern_t;

int main(void){
    pattern_t patterns[] = {
        {"tiny   (1..64 bytes)",     1,    64},
        {"small  (1..256 bytes)",    1,   256},
        {"medium (1..1024 bytes)",   1,  1024},
        {"fixed  (24 bytes)",       24,    24},
        {"fixed  (100 bytes)",     100,   100},
        {"fixed  (1000 bytes)",   1000,  1000},
    };

    printf("alignment = %d bytes\n", MY_ALLOC_ALIGNMENT);
    printf("%-26s %12s %12s %12s\n", "request pattern", "requested", "used", "utilization");

    for(unsigned p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++){
        my_heap_init();
        srand(7);

        void** ptrs = malloc(N * sizeof(void*));
        size_t requested = 0;
        for(int i = 0; i < N; i++){
            size_t range = patterns[p].hi - patterns[p].lo + 1;
            size_t size = patterns[p].lo + (size_t)rand() % range;
            ptrs[i] = my_malloc(size);
            requested += size;
        }

        my_heap_stats_t s;
        my_heap_stats(&s);
        printf("%-26s %12zu %12zu %11.1f%%\n", patterns[p].name, requested, s.used_bytes,
               100.0 * (double)requested / (double)s.used_bytes);

        for(int i = 0; i < N; i++) my_free(ptrs[i]);
        free(ptrs);
    }
    return 0;
}
