#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "include/malloc.h"

/*
 * FORK TEST
 * ---------
 * Several threads free blocks (taking the main thread's arena lock) while the main thread forks again and again.
 * Each child allocates a bit and exits. Without the pthread_atfork handlers, a fork that lands while
 * another thread holds an arena lock gives the child a lock nobody will ever release, and its first
 * malloc hangs forever. A watchdog (alarm) turns that hang into a clear FAIL instead of a stuck test.
 */
#define WORKERS 3
#define FORKS 400

static volatile int stop = 0;

/*
 * Why the main thread's blocks go to the workers: every thread allocates from its OWN arena, so
 * the workers' own malloc/free would never touch the arena the forking thread (and then its child)
 * uses. A lock is only held across a fork when another thread is working in YOUR arena, and that
 * is exactly what a cross-thread free does. So the main thread allocates blocks, the workers free
 * them (locking main's arena while they do), and main forks while that is going on.
 */
#define SHARED_SLOTS 256
static void* shared_slots[SHARED_SLOTS];

static void* hammer(void* arg){
    (void)arg;
    while(!__atomic_load_n(&stop, __ATOMIC_RELAXED)){
        for(int i = 0; i < SHARED_SLOTS; i++){
            //take whatever main left in this slot (leaving NULL behind) and free it from THIS thread
            void* p = __atomic_exchange_n(&shared_slots[i], NULL, __ATOMIC_ACQ_REL);
            if(p != NULL){ my_free(p); }
        }
    }
    return NULL;
}

static void on_alarm(int sig){
    (void)sig;
    const char msg[] = "FAIL: fork test hung (a child waited forever for a lock)\n";
    ssize_t ignored = write(1, msg, sizeof(msg) - 1);
    (void)ignored;
    _exit(2);
}

int main(void){
    signal(SIGALRM, on_alarm);
    alarm(60);

    pthread_t threads[WORKERS];
    for(long i = 0; i < WORKERS; i++){ pthread_create(&threads[i], NULL, hammer, (void*)(i + 1)); }

    int failures = 0;
    for(int n = 0; n < FORKS; n++){
        //refill the slots with fresh blocks from main's arena for the workers to free
        for(int i = 0; i < SHARED_SLOTS; i++){
            if(__atomic_load_n(&shared_slots[i], __ATOMIC_ACQUIRE) == NULL){
                void* p = my_malloc((size_t)(i % 200 + 1));
                void* expected = NULL;
                if(!__atomic_compare_exchange_n(&shared_slots[i], &expected, p, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)){
                    my_free(p);
                }
            }
        }

        pid_t pid = fork();
        if(pid == 0){
            //child: the allocator must still work. (only one thread exists in the child)
            void* ptrs[50];
            for(int i = 0; i < 50; i++){ ptrs[i] = my_malloc((size_t)(i * 13 + 1)); if(ptrs[i] == NULL) _exit(3); memset(ptrs[i], 1, (size_t)(i * 13 + 1)); }
            for(int i = 0; i < 50; i++){ my_free(ptrs[i]); }
            _exit(0);
        }
        int status = 0;
        waitpid(pid, &status, 0);
        if(!WIFEXITED(status) || WEXITSTATUS(status) != 0){ failures++; }
    }

    __atomic_store_n(&stop, 1, __ATOMIC_RELAXED);
    for(int i = 0; i < WORKERS; i++){ pthread_join(threads[i], NULL); }
    for(int i = 0; i < SHARED_SLOTS; i++){ my_free(shared_slots[i]); }

    int healthy = my_heap_check();
    printf("%d forks while %d threads free into main's arena: %s\n", FORKS, WORKERS, (failures == 0 && healthy) ? "PASS" : "FAIL");
    return (failures == 0 && healthy) ? 0 : 1;
}
