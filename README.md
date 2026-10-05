# Multi-threaded heap allocator

A thread-safe `malloc`/`free` in C with per-thread arenas, power-of-two size bins,
block splitting/coalescing, 64-byte aligned payloads, and a heap consistency checker.

## Build and test
    make test            # single-threaded + multithreaded stress tests
    make test-align      # same tests at 16/32/64/128 byte alignment
    make tsan            # multithreaded test under ThreadSanitizer
    make tsan-nolock     # same, lock removed: TSan should report races
    make bench           # fragmented-heap benchmark vs glibc
    make bench-memory    # memory overhead at 16/32/64 byte alignment

## Design (see the big comment at the top of malloc.c)
- Block = header(32) | payload | footer(16). Footer lets us find the previous block to merge.
- Chunks come from mmap, fenced by a prologue and epilogue so walking never leaves a chunk.
- Each thread gets an arena (own lock, own bins). Cross-thread free locks the OWNER's arena.
- Free lists: 32 power-of-two bins, doubly linked, with a bitmap of non-empty bins.
- Requests >= 128 KB get their own mmap.
- Payloads are aligned to MY_ALLOC_ALIGNMENT (default 64).

## Known limitations
- Chunks are never returned to the OS; arenas are not recycled on thread exit.
- Past 64 threads, arenas are shared. A full arena does not borrow from another.
- Double free of a large block is not detected.
- Small allocations carry 48 bytes of header/footer plus alignment rounding.
- No realloc/calloc yet, and not yet built as an LD_PRELOAD shared library.
