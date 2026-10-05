#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sched.h>
#include <time.h>

/*
 * THREAD SCALING BENCHMARK
 * ------------------------
 * This program only ever calls plain malloc / free / realloc. It does NOT know which allocator it
 * is running on. run_bench.sh runs the SAME binary three ways:
 *      1. normally                              -> glibc malloc
 *      2. LD_PRELOAD=libmyalloc.so              -> our per-thread-arena allocator
 *      3. LD_PRELOAD=libmyalloc_single.so       -> our allocator with ONE shared arena (= one global lock)
 * so the only thing that changes between runs is the allocator.
 *
 * Usage: ./bench_threads <workload> <threads> [ops_per_thread]
 * Prints one line:  <workload> <threads> <total ops> <seconds> <million ops/sec> <peak RSS in KB>
 *
 * Workloads:
 *   private   each thread allocates/frees its OWN blocks (best case for per-thread arenas)
 *   handoff   thread i allocates, thread i+1 frees (every free is a cross-thread free: the hard case)
 *   realloc   each thread grows buffers with realloc, like a dynamic array
 */

#define RING 4096
#define SLOTS 1024

static int num_threads = 4;
static long ops_per_thread = 2000000;
static int workload = 0;                 //0 private, 1 handoff, 2 realloc
static pthread_barrier_t start_barrier;

//tiny fast random number generator (xorshift), one state per thread so threads never share anything
static inline uint64_t next_random(uint64_t* state){
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

//mostly small sizes, a few medium, rarely bigger: roughly what real programs ask for
static inline size_t random_size(uint64_t* state){
    uint64_t r = next_random(state);
    uint64_t pick = r % 100;
    uint64_t v = (r >> 8);
    if(pick < 70) return 16 + v % 113;       //16..128
    if(pick < 95) return 129 + v % 896;      //129..1024
    return 1025 + v % 7168;                  //1025..8192
}

/* single producer, single consumer queue of pointers (lock free, so the queue itself costs almost nothing) */
typedef struct {
    _Alignas(64) atomic_size_t head;         //next slot the consumer reads
    _Alignas(64) atomic_size_t tail;         //next slot the producer writes
    _Alignas(64) void* slots[RING];
} ring_t;
static ring_t rings[64];

static int ring_push(ring_t* r, void* p){
    size_t t = atomic_load_explicit(&r->tail, memory_order_relaxed);
    size_t h = atomic_load_explicit(&r->head, memory_order_acquire);
    if(t - h == RING) return 0;
    r->slots[t % RING] = p;
    atomic_store_explicit(&r->tail, t + 1, memory_order_release);
    return 1;
}

static void* ring_pop(ring_t* r){
    size_t h = atomic_load_explicit(&r->head, memory_order_relaxed);
    size_t t = atomic_load_explicit(&r->tail, memory_order_acquire);
    if(h == t) return NULL;
    void* p = r->slots[h % RING];
    atomic_store_explicit(&r->head, h + 1, memory_order_release);
    return p;
}

static void work_private(int id){
    uint64_t rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(id + 1);
    void* slots[SLOTS] = {0};
    for(long i = 0; i < ops_per_thread; i++){
        size_t s = (size_t)(next_random(&rng) % SLOTS);
        if(slots[s] != NULL){
            free(slots[s]);
            slots[s] = NULL;
        }
        else{
            size_t size = random_size(&rng);
            char* p = malloc(size);
            if(p == NULL){ fprintf(stderr, "out of memory\n"); exit(1); }
            p[0] = 1;                       //touch it, like a real program would
            p[size - 1] = 1;
            slots[s] = p;
        }
    }
    for(int i = 0; i < SLOTS; i++) free(slots[i]);
}

static void work_handoff(int id){
    uint64_t rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(id + 1);
    ring_t* out = &rings[id];                                   //we produce into this one
    ring_t* in = &rings[(id + num_threads - 1) % num_threads];  //and consume (free) the previous thread's
    long produced = 0, consumed = 0;
    long items = ops_per_thread / 2;                            //one malloc + one free per item

    while(produced < items || consumed < items){
        int progress = 0;
        if(produced < items){
            size_t size = random_size(&rng);
            char* p = malloc(size);
            if(p == NULL){ fprintf(stderr, "out of memory\n"); exit(1); }
            p[0] = 1;
            p[size - 1] = 1;
            if(ring_push(out, p)){ produced++; progress = 1; }
            else free(p);                                       //queue full: undo and try again later
        }
        if(consumed < items){
            void* q = ring_pop(in);
            if(q != NULL){ free(q); consumed++; progress = 1; }
        }
        if(!progress) sched_yield();
    }
}

static void work_realloc(int id){
    uint64_t rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(id + 1);
    long done = 0;
    while(done < ops_per_thread){
        //grow one buffer from 16 bytes to about 64 KB by 1.5x steps, then start over
        size_t size = 16 + next_random(&rng) % 32;
        char* p = malloc(size);
        done++;
        while(size < 65536 && done < ops_per_thread){
            size = size + size / 2 + 1;
            p = realloc(p, size);
            if(p == NULL){ fprintf(stderr, "out of memory\n"); exit(1); }
            p[size - 1] = 1;
            done++;
        }
        free(p);
    }
}

static void* thread_main(void* arg){
    int id = (int)(long)arg;
    pthread_barrier_wait(&start_barrier);       //everyone starts together
    if(workload == 0) work_private(id);
    else if(workload == 1) work_handoff(id);
    else work_realloc(id);
    return NULL;
}

//peak resident memory of the whole process, in KB (VmHWM in /proc/self/status)
static long peak_rss_kb(void){
    FILE* f = fopen("/proc/self/status", "r");
    if(f == NULL) return -1;
    char line[256];
    long kb = -1;
    while(fgets(line, sizeof(line), f)){
        if(strncmp(line, "VmHWM:", 6) == 0){ kb = atol(line + 6); break; }
    }
    fclose(f);
    return kb;
}

int main(int argc, char** argv){
    if(argc < 3){
        fprintf(stderr, "usage: %s <private|handoff|realloc> <threads> [ops_per_thread]\n", argv[0]);
        return 2;
    }
    const char* name = argv[1];
    if(strcmp(name, "private") == 0) workload = 0;
    else if(strcmp(name, "handoff") == 0) workload = 1;
    else if(strcmp(name, "realloc") == 0) workload = 2;
    else { fprintf(stderr, "unknown workload %s\n", name); return 2; }

    num_threads = atoi(argv[2]);
    if(num_threads < 1 || num_threads > 64){ fprintf(stderr, "threads must be 1..64\n"); return 2; }
    if(workload == 1 && num_threads < 2){ fprintf(stderr, "handoff needs at least 2 threads\n"); return 2; }
    if(argc > 3) ops_per_thread = atol(argv[3]);
    else if(workload == 2) ops_per_thread = 400000;

    pthread_barrier_init(&start_barrier, NULL, (unsigned)num_threads + 1);
    pthread_t threads[64];
    for(long i = 0; i < num_threads; i++) pthread_create(&threads[i], NULL, thread_main, (void*)i);

    struct timespec t0, t1;
    pthread_barrier_wait(&start_barrier);       //main joins the barrier so the clock starts as they start
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for(int i = 0; i < num_threads; i++) pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    double seconds = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double total_ops = (double)num_threads * (double)ops_per_thread;
    printf("%s %d %.0f %.4f %.3f %ld\n", name, num_threads, total_ops, seconds, total_ops / seconds / 1e6, peak_rss_kb());
    return 0;
}
