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
 * (Re)initializes the allocator's internal state. Must be called once
 * before any my_malloc/my_free calls. Tests call this in their setup
 * so each test starts from a clean heap.
 *
 * Milestone 1.
 */
void my_heap_init(void);

/*
 * my_malloc / my_free
 * --------------------
 * Drop-in replacements for malloc()/free(), backed entirely by memory
 * this allocator requested from the OS via sbrk() — never by calling
 * the real malloc/free internally.
 */
void *my_malloc(size_t size);
void  my_free(void *ptr);

/*
 * my_realloc — stretch goal, Milestone 7. Not required for the core
 * resume bullets, implement last if you have time.
 */
void *my_realloc(void *ptr, size_t size);

/*
 * my_heap_check
 * -------------
 * Debug/consistency checker: walks the entire heap (not just the free
 * list) and verifies invariants (header/footer sizes match, no two
 * adjacent free blocks, free-list only contains blocks marked free,
 * etc). Returns 1 if healthy, 0 and prints a diagnostic otherwise.
 *
 * You will write this in Milestone 5 — it is your own mini-Valgrind,
 * and writing it is what makes debugging heap corruption tractable
 * instead of guesswork.
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
