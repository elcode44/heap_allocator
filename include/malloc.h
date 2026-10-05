#ifndef MYALLOC_H
#define MYALLOC_H

#include <stddef.h>

/*
 * MY_ALLOC_ALIGNMENT
 * ------------------
 * Every pointer my_malloc returns is a multiple of this many bytes.
 * 64 = one cache line / one AVX-512 vector. Override at build time with
 * -DMY_ALLOC_ALIGNMENT=16 (what the real malloc uses), 32, 64 or 128.
 */
#ifndef MY_ALLOC_ALIGNMENT
#define MY_ALLOC_ALIGNMENT 64
#endif

/*
 * my_heap_init
 * ------------
 * Throws away everything the allocator knows and starts over (memory goes back to the OS).
 * Tests call this in their setup so each test starts from a clean heap. Only call it when no
 * other thread is using the allocator. Normal programs never need it.
 */
void my_heap_init(void);

/*
 * my_malloc / my_free / my_realloc / my_calloc
 * ---------------------------------------------
 * Drop-in replacements for malloc/free/realloc/calloc, backed entirely by memory this
 * allocator maps from the OS with mmap(), never by calling the real malloc internally.
 *   my_malloc(0)  returns NULL (the libmyalloc.so wrappers return a real pointer instead)
 *   my_realloc    resizes in place when it can, and follows the usual rules:
 *                 realloc(NULL, n) == malloc(n), realloc(p, 0) frees p and returns NULL,
 *                 and on failure the old block is left untouched
 *   my_calloc     returns zeroed memory and fails (NULL) if count * size overflows
 */
void *my_malloc(size_t size);
void  my_free(void *ptr);
void *my_realloc(void *ptr, size_t size);
void *my_calloc(size_t count, size_t size);

/*
 * my_memalign
 * -----------
 * Like memalign: returns memory whose address is a multiple of "alignment" (a power of two).
 * Alignments up to MY_ALLOC_ALIGNMENT cost nothing extra; bigger ones cost at least a page.
 * Free it with my_free like any other block.
 */
void *my_memalign(size_t alignment, size_t size);

/*
 * my_usable_size
 * --------------
 * How many bytes you may actually use in this block (can be more than you asked for).
 * Returns 0 for NULL or a block that is already free.
 */
size_t my_usable_size(void *ptr);

/*
 * my_heap_check
 * -------------
 * Debug/consistency checker: walks the entire heap (not just the free
 * lists) and verifies invariants (header/footer sizes match, no two
 * adjacent free blocks, every free list only holds free blocks of the
 * right size, etc). Returns 1 if healthy, 0 and prints a diagnostic otherwise.
 */
int my_heap_check(void);

/*
 * my_heap_stats
 * -------------
 * Snapshot of memory usage across all arenas (used by tests and benchmarks).
 *   mapped_bytes       - memory the allocator has taken from the OS (and not returned)
 *   free_bytes         - payload bytes sitting in free blocks, ready to reuse
 *   free_blocks        - how many separate free blocks there are
 *   largest_free_block - payload size of the biggest free block (low = fragmented)
 *   arenas_with_memory - how many arenas have mapped at least one chunk
 *   used_bytes         - bytes taken by blocks in use, INCLUDING their header and footer
 *   used_blocks        - how many blocks are in use (arena blocks only)
 */
typedef struct {
    size_t mapped_bytes;
    size_t free_bytes;
    size_t free_blocks;
    size_t largest_free_block;
    int    arenas_with_memory;
    size_t used_bytes;
    size_t used_blocks;
} my_heap_stats_t;

void my_heap_stats(my_heap_stats_t* stats);

#endif /* MYALLOC_H */
