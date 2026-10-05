CC = gcc
CFLAGS = -Wall -Wextra -g -O0
MTFLAGS = -Wall -Wextra -g -O1 -pthread

all: main tests tests_mt

main: main.c malloc.c include/malloc.h
	$(CC) $(CFLAGS) -pthread -o main main.c malloc.c

tests: tests.c malloc.c include/malloc.h
	$(CC) $(CFLAGS) -pthread -o tests tests.c malloc.c

tests_mt: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -O2 -o tests_mt tests_mt.c malloc.c

# single threaded tests + multithreaded stress test
test: tests tests_mt
	./tests
	./tests_mt

# run the single threaded + multithreaded tests with several alignments. proves the layout math works for each
test-align:
	for a in 16 32 64 128; do \
	  echo "=== MY_ALLOC_ALIGNMENT=$$a ==="; \
	  $(CC) $(CFLAGS) -pthread -DMY_ALLOC_ALIGNMENT=$$a -o tests_a tests.c malloc.c && ./tests_a 2>/dev/null | tail -2; \
	  $(CC) $(MTFLAGS) -O2 -DMY_ALLOC_ALIGNMENT=$$a -o tests_mt_a tests_mt.c malloc.c && ./tests_mt_a 20000 | tail -2; \
	done

# same stress test, built with ThreadSanitizer (finds data races). smaller run since TSan is slow
tsan: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -fsanitize=thread -o tests_mt_tsan tests_mt.c malloc.c
	./tests_mt_tsan 2000

# the SAME code with the lock removed. TSan should report races. proves the lock is doing real work
tsan-nolock: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -fsanitize=thread -DMY_ALLOC_NO_LOCK -o tests_mt_nolock tests_mt.c malloc.c
	-./tests_mt_nolock 2000

# fragmented heap benchmark: our allocator vs the real malloc
bench: bench_freelist.c malloc.c include/malloc.h
	$(CC) -O2 -pthread -o bench bench_freelist.c malloc.c
	$(CC) -O2 -DUSE_GLIBC -o bench_glibc bench_freelist.c
	./bench
	./bench_glibc

# how much memory does alignment cost? same workloads at 16, 32 and 64 byte alignment
bench-memory: bench_memory.c malloc.c include/malloc.h
	for a in 16 32 64; do \
	  $(CC) -O2 -pthread -DMY_ALLOC_ALIGNMENT=$$a -o bench_memory_$$a bench_memory.c malloc.c && ./bench_memory_$$a; echo; \
	done

clean:
	rm -f main tests tests_mt tests_mt_tsan tests_mt_nolock bench bench_glibc tests_a tests_mt_a bench_memory_*

.PHONY: all test test-align tsan tsan-nolock bench bench-memory clean
