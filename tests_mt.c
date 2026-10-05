#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <time.h>
#include "include/malloc.h"

/*
 * MULTITHREADED STRESS TEST
 * -------------------------
 * Phase 1 (private):  every thread randomly mallocs/frees its own blocks and
 *                     checks that nobody else overwrote them.
 * Phase 2 (hand-off): thread i keeps allocating blocks and passing them to
 *                     thread i+1, which frees them WHILE thread i is still
 *                     allocating. This is the "cross-thread free" case, the one
 *                     real allocators have to be most careful about.
 * If the lock were missing, the free list would be corrupted and either a
 * pattern check, my_heap_check, or ThreadSanitizer would catch it.
 */

#define SLOTS 128
#define MAX_THREADS 8
#define MAILBOX_CAP 64

static int iterations = 20000;          //private operations per thread (phase 1)
static int handoff_items = 10000;       //blocks each thread sends to its neighbor (phase 2)
static int num_threads = 4;
static pthread_barrier_t barrier;       //makes all threads start phase 2 together
static int thread_failed[MAX_THREADS];  //each thread writes only its own entry, so no race

//a small queue one thread uses to pass blocks to its neighbor. the queue has its OWN lock;
//that lock only protects the queue, it says nothing about the allocator's internals
typedef struct {
    pthread_mutex_t lock;
    void* items[MAILBOX_CAP];
    size_t sizes[MAILBOX_CAP];
    int head;
    int count;
} mailbox_t;
static mailbox_t mailbox[MAX_THREADS];

static int mailbox_push(mailbox_t* box, void* p, size_t size){
    int pushed = 0;
    pthread_mutex_lock(&box -> lock);
    if(box -> count < MAILBOX_CAP){
        int tail = (box -> head + box -> count) % MAILBOX_CAP;
        box -> items[tail] = p;
        box -> sizes[tail] = size;
        box -> count++;
        pushed = 1;
    }
    pthread_mutex_unlock(&box -> lock);
    return pushed;
}

static int mailbox_pop(mailbox_t* box, void** p, size_t* size){
    int popped = 0;
    pthread_mutex_lock(&box -> lock);
    if(box -> count > 0){
        *p = box -> items[box -> head];
        *size = box -> sizes[box -> head];
        box -> head = (box -> head + 1) % MAILBOX_CAP;
        box -> count--;
        popped = 1;
    }
    pthread_mutex_unlock(&box -> lock);
    return popped;
}

//check every byte of a block still holds the pattern we wrote
static int pattern_ok(void* p, size_t size, unsigned char tag){
    unsigned char* bytes = p;
    for(size_t i = 0; i < size; i++){
        if(bytes[i] != tag) return 0;
    }
    return 1;
}

static void* worker(void* arg){
    int id = (int)(long)arg;
    unsigned int seed = 1234 + id;      //rand_r keeps a private seed per thread

    /* PHASE 1: private random alloc/free */
    void* ptrs[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};
    unsigned char tags[SLOTS] = {0};

    for(int i = 0; i < iterations; i++){
        int slot = rand_r(&seed) % SLOTS;

        if(ptrs[slot] != NULL){
            if(!pattern_ok(ptrs[slot], sizes[slot], tags[slot])) thread_failed[id] = 1;
            my_free(ptrs[slot]);
            ptrs[slot] = NULL;
        }
        else{
            size_t size = (size_t)(rand_r(&seed) % 400 + 1);
            void* p = my_malloc(size);
            if(p == NULL){ thread_failed[id] = 1; continue; }
            if((uintptr_t)p % MY_ALLOC_ALIGNMENT != 0) thread_failed[id] = 1;
            tags[slot] = (unsigned char)(rand_r(&seed) % 255 + 1);
            memset(p, tags[slot], size);
            ptrs[slot] = p;
            sizes[slot] = size;
        }

        //thread 0 occasionally checks the whole heap while the others keep hammering it
        if(id == 0 && i % 2000 == 0){
            if(!my_heap_check()) thread_failed[id] = 1;
        }
    }
    for(int i = 0; i < SLOTS; i++) my_free(ptrs[i]);

    /* PHASE 2: producer / consumer ring.
     * every thread keeps ALLOCATING from its own arena while ALSO freeing blocks that the
     * previous thread allocated. so each arena is being used by its owner and by a
     * cross-thread freer at the same moment: the hard case for an allocator */
    pthread_barrier_wait(&barrier);

    int from = (id + num_threads - 1) % num_threads;   //the thread whose blocks we free
    int produced = 0;
    int consumed = 0;

    while(produced < handoff_items || consumed < handoff_items){
        int before = produced + consumed;

        if(produced < handoff_items){
            size_t size = (size_t)(rand_r(&seed) % 300 + 1);
            void* p = my_malloc(size);
            if(p == NULL){ thread_failed[id] = 1; produced = handoff_items; }
            else{
                memset(p, id + 1, size);               //tag = the id of the thread that allocated it
                if(mailbox_push(&mailbox[id], p, size)) produced++;
                else my_free(p);                       //mailbox full: undo and retry later
            }
        }

        void* q;
        size_t qsize;
        if(consumed < handoff_items && mailbox_pop(&mailbox[from], &q, &qsize)){
            if(!pattern_ok(q, qsize, (unsigned char)(from + 1))) thread_failed[id] = 1;
            my_free(q);                                //free memory another thread allocated
            consumed++;
        }

        //nothing to do right now (waiting on a neighbor): let another thread run instead of spinning
        if(produced + consumed == before){
            sched_yield();
        }
    }
    return NULL;
}

//runs one round with n threads, returns 1 if everything passed
static int run(int n, double* ops_per_sec, int* arenas_used){
    num_threads = n;
    memset(thread_failed, 0, sizeof(thread_failed));
    my_heap_init();
    pthread_barrier_init(&barrier, NULL, n);
    for(int i = 0; i < n; i++){
        pthread_mutex_init(&mailbox[i].lock, NULL);
        mailbox[i].head = 0;
        mailbox[i].count = 0;
    }

    pthread_t threads[MAX_THREADS];
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for(long i = 0; i < n; i++) pthread_create(&threads[i], NULL, worker, (void*)i);
    for(int i = 0; i < n; i++) pthread_join(threads[i], NULL);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    pthread_barrier_destroy(&barrier);

    double seconds = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    //each phase 1 iteration is one malloc OR one free; phase 2 is one malloc + one free per item
    double total_ops = (double)n * (iterations + 2.0 * handoff_items);
    *ops_per_sec = total_ops / seconds;

    int ok = my_heap_check();       //single threaded now, everything should be merged and consistent
    for(int i = 0; i < n; i++) if(thread_failed[i]) ok = 0;

    //every thread should have been given its own arena
    my_heap_stats_t stats;
    my_heap_stats(&stats);
    *arenas_used = stats.arenas_with_memory;
    if(stats.arenas_with_memory != n) ok = 0;
    return ok;
}

int main(int argc, char** argv){
    if(argc > 1) iterations = atoi(argv[1]);   //TSan runs are slow, let the caller shrink the test
    handoff_items = iterations / 2;

    int counts[] = {1, 2, 4, 8};
    int all_ok = 1;
    for(int c = 0; c < 4; c++){
        double ops;
        int arenas_used;
        int ok = run(counts[c], &ops, &arenas_used);
        printf("%d thread(s): %s   %.2f M ops/sec   (%d arenas in use)\n", counts[c], ok ? "PASS" : "FAIL", ops / 1e6, arenas_used);
        if(!ok) all_ok = 0;
    }
    printf("\n%s\n", all_ok ? "ALL MULTITHREADED TESTS PASSED" : "MULTITHREADED TESTS FAILED");
    return all_ok ? 0 : 1;
}
