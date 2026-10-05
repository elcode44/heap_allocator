CC = gcc
CFLAGS = -Wall -Wextra -g -O0
MTFLAGS = -Wall -Wextra -g -O1 -pthread

all: main tests tests_mt tests_fork

main: main.c malloc.c include/malloc.h
	$(CC) $(CFLAGS) -pthread -o main main.c malloc.c

tests: tests.c malloc.c include/malloc.h
	$(CC) $(CFLAGS) -pthread -o tests tests.c malloc.c

tests_mt: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -O2 -o tests_mt tests_mt.c malloc.c

# MY_ALLOC_TEST_SLOW_LOCK makes threads linger inside the lock so a fork reliably lands while it is held
tests_fork: tests_fork.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -O2 -DMY_ALLOC_TEST_SLOW_LOCK -o tests_fork tests_fork.c malloc.c

# single threaded tests + multithreaded stress test + fork test + real programs on the preload library
test: tests tests_mt tests_fork libmyalloc.so
	./tests
	./tests_mt
	./tests_fork
	sh ./test_preload.sh

# run the single threaded + multithreaded tests with several alignments. proves the layout math works for each
test-align:
	for a in 16 32 64 128; do \
	  echo "=== MY_ALLOC_ALIGNMENT=$$a ==="; \
	  $(CC) $(CFLAGS) -pthread -DMY_ALLOC_ALIGNMENT=$$a -o tests_a tests.c malloc.c && ./tests_a 2>/dev/null | tail -2; \
	  $(CC) $(MTFLAGS) -O2 -DMY_ALLOC_ALIGNMENT=$$a -o tests_mt_a tests_mt.c malloc.c && ./tests_mt_a 20000 | tail -2; \
	done

# the allocator as a shared library you can slide under any program:
#   LD_PRELOAD=./libmyalloc.so ls
# -fno-builtin-*: stop gcc from "optimizing" our malloc/free (it knows what the real ones do)
LIBFLAGS = -O2 -g -fPIC -shared -pthread -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free

lib: libmyalloc.so

libmyalloc.so: malloc.c preload.c include/malloc.h
	$(CC) $(LIBFLAGS) -Wall -Wextra -o libmyalloc.so malloc.c preload.c

# same library, but every thread shares ONE arena = one global lock. the baseline for the benchmark
libmyalloc_single.so: malloc.c preload.c include/malloc.h
	$(CC) $(LIBFLAGS) -DMY_ALLOC_SINGLE_ARENA -Wall -Wextra -o libmyalloc_single.so malloc.c preload.c

# run real programs on the library (checks they behave and that our allocator really was used)
test-preload: libmyalloc.so
	sh ./test_preload.sh

# same stress test, built with ThreadSanitizer (finds data races). smaller run since TSan is slow
tsan: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -fsanitize=thread -o tests_mt_tsan tests_mt.c malloc.c
	./tests_mt_tsan 2000

# the SAME code with the lock removed. TSan should report races. proves the lock is doing real work
tsan-nolock: tests_mt.c malloc.c include/malloc.h
	$(CC) $(MTFLAGS) -fsanitize=thread -DMY_ALLOC_NO_LOCK -o tests_mt_nolock tests_mt.c malloc.c
	-timeout 60 ./tests_mt_nolock 2000

# fragmented heap benchmark: our allocator vs the real malloc
bench: bench_freelist.c malloc.c include/malloc.h
	$(CC) -O2 -pthread -o bench bench_freelist.c malloc.c
	$(CC) -O2 -DUSE_GLIBC -o bench_glibc bench_freelist.c
	./bench
	./bench_glibc

bench_threads: bench_threads.c
	$(CC) -O2 -Wall -Wextra -pthread -o bench_threads bench_threads.c

# thread scaling benchmark: glibc vs this allocator vs this allocator with one global lock
bench-threads:
	sh ./run_bench.sh

# how much memory does alignment cost? same workloads at 16, 32 and 64 byte alignment
bench-memory: bench_memory.c malloc.c include/malloc.h
	for a in 16 32 64; do \
	  $(CC) -O2 -pthread -DMY_ALLOC_ALIGNMENT=$$a -o bench_memory_$$a bench_memory.c malloc.c && ./bench_memory_$$a; echo; \
	done

clean:
	rm -f bench_threads libmyalloc.so libmyalloc_single.so main tests tests_mt tests_fork tests_mt_tsan tests_mt_nolock bench bench_glibc tests_a tests_mt_a bench_memory_*

.PHONY: all test test-preload lib bench-threads test-align tsan tsan-nolock bench bench-memory clean
