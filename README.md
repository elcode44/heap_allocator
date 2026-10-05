# Multi-threaded heap allocator

A thread-safe `malloc` / `free` / `calloc` / `realloc` in C, built from scratch on `mmap`:
per-thread arenas, power-of-two size bins, block splitting and coalescing, 64-byte aligned
payloads, a heap consistency checker, and an `LD_PRELOAD` build that runs real programs on it.

Everything is explained in comments, starting with the big diagram at the top of `malloc.c`.

## Quick start (Linux, or Windows through WSL2)

    sudo apt install -y build-essential     # gcc + make (ThreadSanitizer ships with gcc)
    make test                               # unit tests, multithreaded stress test, fork test, real programs
    make tsan                               # the multithreaded test under ThreadSanitizer
    make bench-threads                      # thread-scaling benchmark vs glibc (takes a few minutes)

Run any program on the allocator:

    make lib
    LD_PRELOAD=./libmyalloc.so ls -R /usr/include
    LD_PRELOAD=./libmyalloc.so MYALLOC_STATS=1 python3 -c "print('hi')"   # prints a summary at exit

## Design

- **Blocks**: `header(32) | payload | footer(16)`. The footer lets a freed block find the block before it, so it can merge with it.
- **Chunks** come from `mmap` (1 MB at a time), fenced by a prologue and an epilogue so walking never leaves a chunk.
- **Arenas**: every thread gets its own heap (own lock, own bins, own chunks), so threads mostly never touch each other's data.
- **Cross-thread free**: each header stores its owner arena's id. `free` locks the *owner's* arena, not the caller's.
- **Size bins**: 32 power-of-two bins, doubly linked, plus a bitmap of non-empty bins. Picking a bin and falling back to a bigger one is constant time; the walk inside one bin is not.
- **Splitting and coalescing**: a big free block is split to fit a request, and neighbours are merged when freed.
- **Large requests** (128 KB and up) get their own `mmap` and go back to the OS with `munmap`.
- **Alignment**: payloads are aligned to `MY_ALLOC_ALIGNMENT` (default 64 = one cache line). Change it with `-DMY_ALLOC_ALIGNMENT=16/32/64/128`.
- **`realloc`** resizes in place when it can (shrink by splitting, grow by swallowing a free neighbour). Big blocks use `mremap`, so no bytes are copied.
- **`calloc`** checks for overflow and skips the `memset` for fresh `mmap` pages (already zero).
- **`memalign`/`posix_memalign`/`aligned_alloc`** work for any power-of-two alignment (alignments above 64 cost at least a page).
- **`fork`** safety: `pthread_atfork` handlers take every lock before a fork and release them in parent and child.

## How it is tested

| Check | Command | What it proves |
|---|---|---|
| Unit tests | `make test` | splitting, coalescing, bins, alignment (16/32/64/128), calloc, realloc, memalign |
| Heap checker | built into every test | after *every* operation in the randomized tests, the whole heap is walked and all invariants verified |
| Multithreaded stress | `make test` | random alloc/realloc/free per thread, plus a ring where thread *i* allocates and thread *i+1* reallocs and frees while *i* keeps allocating |
| ThreadSanitizer | `make tsan` | no data races with locks on; `make tsan-nolock` removes the locks and TSan reports races, so the check can fail |
| Fork test | `make test` | 400 forks while other threads hold the parent's arena lock; the child must not hang |
| Real programs | `make test-preload` | `ls`, `find`, `sort`, `awk`, `sed`, `gzip`, `tar`, `perl`, `python3` (threads), `gcc` produce output identical to a normal run |
| Alignment sweep | `make test-align` | the layout math works for 16, 32, 64 and 128 byte alignment |

The tests were also checked by injecting bugs on purpose (forget to unlink a block, skip a merge, drop an overflow check,
remove the fork handlers, and so on) and confirming that a test fails each time.

## Benchmarking

`make bench-threads` runs one benchmark program three ways, so only the allocator changes:

1. `glibc`: the normal `malloc`
2. `myalloc`: `LD_PRELOAD=libmyalloc.so`, one arena per thread
3. `myalloc-1lock`: `LD_PRELOAD=libmyalloc_single.so`, every thread forced into one arena (one global lock)

at 1, 2, 4 and 8 threads, 5 repetitions each, on three workloads: `private` (each thread frees its own blocks),
`handoff` (every free is a cross-thread free) and `realloc` (growing buffers). It writes `results/results.csv`,
`results/machine.txt` and a markdown table to `results/summary.md`.

Thread counts above the machine's core count are flagged: past that point the numbers show OS scheduling, not allocator scaling.
Report the CPU and core count with any number you quote. Results go here once measured on a multicore machine.

Other benchmarks: `make bench` (fragmented heap vs glibc) and `make bench-memory` (memory overhead at 16/32/64 byte alignment).

## Known limitations

- Chunks are never returned to the OS, and an arena is not recycled when its thread exits.
- Past 64 threads, arenas are shared. A full arena does not borrow memory from another.
- Small allocations are expensive in memory: 48 bytes of header and footer plus rounding to the alignment
  (a 24-byte request uses 128 bytes at 64-byte alignment). See `make bench-memory`.
- Double free of a large block is not detected (its memory is already unmapped), and a pointer that did not come
  from this allocator is only caught by cheap checks.
- Alignment guarantees where a payload *starts*; the last cache line of a small block can still be shared with the next
  block's header, so it is not a complete false-sharing cure.
- Not tuned against production allocators (glibc, jemalloc, tcmalloc): no thread caches, no per-size-class locks.
