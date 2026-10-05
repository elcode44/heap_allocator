#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "include/malloc.h"

//tiny test helper: prints PASS/FAIL and counts failures
static int failures = 0;
#define CHECK(condition, name) do { \
    if(condition){ printf("PASS: %s\n", name); } \
    else { printf("FAIL: %s\n", name); failures++; } \
} while(0)


//every pointer must be 16 byte aligned and usable
static void test_basic(void){
    my_heap_init();
    void* a = my_malloc(1);
    void* b = my_malloc(100);
    void* c = my_malloc(5000);

    CHECK(a && b && c, "basic: allocations succeed");
    CHECK((uintptr_t)a % MY_ALLOC_ALIGNMENT == 0 && (uintptr_t)b % MY_ALLOC_ALIGNMENT == 0 &&
          (uintptr_t)c % MY_ALLOC_ALIGNMENT == 0, "basic: pointers are aligned");
    CHECK(my_malloc(0) == NULL, "basic: malloc(0) returns NULL");

    memset(a, 0xAA, 1);
    memset(b, 0xBB, 100);
    memset(c, 0xCC, 5000);
    CHECK(my_heap_check(), "basic: heap healthy after writes");

    my_free(a); my_free(b); my_free(c);
    CHECK(my_heap_check(), "basic: heap healthy after frees");
}

//free a block then ask for the same size, we should get the same spot back
static void test_reuse(void){
    my_heap_init();
    void* a = my_malloc(64);
    void* keep = my_malloc(64);   //stops a from merging into the big free block
    my_free(a);
    void* b = my_malloc(64);
    CHECK(a == b, "reuse: freed block is reused");
    my_free(b); my_free(keep);
    CHECK(my_heap_check(), "reuse: heap healthy");
}

//free three neighbors in a bad order, they must merge into one block
static void test_coalesce(void){
    my_heap_init();
    void* a = my_malloc(100);
    void* b = my_malloc(100);
    void* c = my_malloc(100);

    my_free(a);
    my_free(c);
    my_free(b);   //middle last: needs to merge with BOTH neighbors
    CHECK(my_heap_check(), "coalesce: heap healthy (no adjacent free blocks)");

    //if everything merged, a request bigger than any single old block fits at a's spot
    my_heap_stats_t before, after;
    my_heap_stats(&before);
    void* big = my_malloc(300);
    my_heap_stats(&after);
    CHECK(big == a, "coalesce: merged block starts where a started");
    CHECK(before.mapped_bytes == after.mapped_bytes, "coalesce: heap did not need to grow");
    my_free(big);
}


//the bins should steer a request to a block of about the right size
static void test_bins(void){
    my_heap_init();

    //keepers sit between the blocks so freed blocks can't merge with each other
    void* small = my_malloc(100);
    void* k1 = my_malloc(16);
    void* big = my_malloc(1000);
    void* k2 = my_malloc(16);

    //free the small one FIRST, then the big one. in a single list the big block would be at the
    //front, and a request for 100 bytes would carve it up. with bins, the small block is found
    my_free(small);
    my_free(big);
    void* again = my_malloc(100);
    CHECK(again == small, "bins: request gets the right-sized block, not the big one");
    my_free(again);

    //now empty the small bin. a request for 100 bytes has to come from a BIGGER bin (found with the bitmap)
    void* from_big = my_malloc(100);
    CHECK(from_big == small || from_big == big, "bins: falls back to a bigger bin");
    CHECK(my_heap_check(), "bins: heap healthy after splitting a bigger bin");

    my_free(from_big); my_free(k1); my_free(k2);
    CHECK(my_heap_check(), "bins: heap healthy at the end");
}


//alignment must hold for every size, and keep holding after blocks are split, merged and reused
static void test_alignment(void){
    my_heap_init();
    int ok = 1;

    //every size from 1 to 700 bytes, all alive at once
    void* ptrs[700];
    for(int i = 0; i < 700; i++){
        ptrs[i] = my_malloc((size_t)(i + 1));
        if(ptrs[i] == NULL || (uintptr_t)ptrs[i] % MY_ALLOC_ALIGNMENT != 0) ok = 0;
    }
    CHECK(ok, "alignment: sizes 1..700 are all aligned");

    //free every other one (creates holes), then allocate different sizes into them (splits)
    for(int i = 0; i < 700; i += 2){ my_free(ptrs[i]); ptrs[i] = NULL; }
    for(int i = 0; i < 700; i += 2){
        ptrs[i] = my_malloc((size_t)((i * 7) % 300 + 1));
        if(ptrs[i] == NULL || (uintptr_t)ptrs[i] % MY_ALLOC_ALIGNMENT != 0) ok = 0;
    }
    CHECK(ok, "alignment: still aligned after splitting and reusing holes");
    CHECK(my_heap_check(), "alignment: heap healthy");

    //free everything (merges), then allocate again across merged blocks
    for(int i = 0; i < 700; i++){ my_free(ptrs[i]); }
    for(int i = 0; i < 700; i++){
        ptrs[i] = my_malloc((size_t)(i % 90 + 1));
        if(ptrs[i] == NULL || (uintptr_t)ptrs[i] % MY_ALLOC_ALIGNMENT != 0) ok = 0;
    }
    CHECK(ok, "alignment: still aligned after merging");
    for(int i = 0; i < 700; i++){ my_free(ptrs[i]); }

    //the memory must really be usable up to the last byte of the request (nothing overlaps)
    char* x = my_malloc(1);
    char* y = my_malloc(1);
    char* z = my_malloc(1000);
    *x = 1; *y = 2; memset(z, 3, 1000);
    CHECK(*x == 1 && *y == 2 && z[999] == 3, "alignment: neighbouring blocks do not overlap");
    my_free(x); my_free(y); my_free(z);
    CHECK(my_heap_check(), "alignment: heap healthy at the end");
}

//used_bytes should count whole blocks (header + payload + footer), each a multiple of the alignment
static void test_block_sizes(void){
    my_heap_init();
    my_heap_stats_t s;

    void* a = my_malloc(1);
    my_heap_stats(&s);
    CHECK(s.used_blocks == 1, "sizes: one block in use");
    CHECK(s.used_bytes % MY_ALLOC_ALIGNMENT == 0, "sizes: block total is a multiple of the alignment");
    size_t one_byte_cost = s.used_bytes;

    //a request that still fits in the same block must cost exactly the same
    void* b = my_malloc(16);
    my_heap_stats(&s);
    CHECK(s.used_bytes - one_byte_cost == one_byte_cost, "sizes: malloc(1) and malloc(16) cost the same");

    my_free(a); my_free(b);
    my_heap_stats(&s);
    CHECK(s.used_blocks == 0 && s.used_bytes == 0, "sizes: nothing in use after freeing");
}

//big requests (these skip the arenas and get their own mmap)
static void test_big(void){
    my_heap_init();
    my_heap_stats_t s;

    void* a = my_malloc(200000);
    void* b = my_malloc(3000000);
    CHECK(a && b, "big: large allocations work");
    CHECK((uintptr_t)a % MY_ALLOC_ALIGNMENT == 0 && (uintptr_t)b % MY_ALLOC_ALIGNMENT == 0,
          "big: large pointers are aligned");
    memset(a, 1, 200000);
    memset(b, 2, 3000000);

    my_heap_stats(&s);
    CHECK(s.mapped_bytes >= 3200000, "big: large blocks are counted as mapped memory");

    my_free(a);
    my_free(b);
    my_heap_stats(&s);
    CHECK(s.mapped_bytes == 0, "big: freeing large blocks returns the memory to the OS");
    CHECK(my_heap_check(), "big: heap healthy");
}

//an arena grows by whole chunks when it runs out of room
static void test_grow(void){
    my_heap_init();
    my_heap_stats_t s;

    //100 KB each is just under the large threshold, so these must come from the arena
    //and will not all fit in one 1 MB chunk
    void* ptrs[30];
    for(int i = 0; i < 30; i++){
        ptrs[i] = my_malloc(100000);
        memset(ptrs[i], i + 1, 100000);
    }
    my_heap_stats(&s);
    CHECK(s.mapped_bytes >= 3000000, "grow: arena mapped several chunks");
    CHECK(my_heap_check(), "grow: heap healthy across multiple chunks");

    int ok = 1;
    for(int i = 0; i < 30; i++){
        unsigned char* bytes = ptrs[i];
        if(bytes[0] != (unsigned char)(i + 1) || bytes[99999] != (unsigned char)(i + 1)) ok = 0;
        my_free(ptrs[i]);
    }
    CHECK(ok, "grow: blocks in different chunks did not overlap");
    CHECK(my_heap_check(), "grow: heap healthy after freeing");
}

//double free and foreign pointer should be rejected, not corrupt the heap
static void test_bad_free(void){
    my_heap_init();
    void* a = my_malloc(32);
    void* keep = my_malloc(32);
    my_free(a);
    my_free(a);          //double free (prints a warning)
    my_free(NULL);       //legal no-op
    CHECK(my_heap_check(), "bad free: heap survives a double free");
    my_free(keep);
}

//random allocs and frees, with a pattern in every block to detect overlap
#define SLOTS 256
#define ITERATIONS 50000
static void test_random(void){
    my_heap_init();
    srand(12345);

    void* ptrs[SLOTS] = {0};
    size_t sizes[SLOTS] = {0};
    unsigned char tags[SLOTS] = {0};
    int ok = 1;

    for(int i = 0; i < ITERATIONS && ok; i++){
        int slot = rand() % SLOTS;

        if(ptrs[slot] != NULL){
            //make sure nobody scribbled on this block while we held it
            unsigned char* bytes = ptrs[slot];
            for(size_t j = 0; j < sizes[slot]; j++){
                if(bytes[j] != tags[slot]){ ok = 0; break; }
            }
            my_free(ptrs[slot]);
            ptrs[slot] = NULL;
        }
        else{
            //mostly small, sometimes big
            size_t size = (rand() % 20 == 0) ? (size_t)(rand() % 30000 + 1) : (size_t)(rand() % 500 + 1);
            void* p = my_malloc(size);
            if(p == NULL){ ok = 0; break; }
            if((uintptr_t)p % MY_ALLOC_ALIGNMENT != 0){ ok = 0; break; }
            tags[slot] = (unsigned char)(rand() % 255 + 1);
            memset(p, tags[slot], size);
            ptrs[slot] = p;
            sizes[slot] = size;
        }

        //check the whole heap after EVERY operation
        if(!my_heap_check()){ ok = 0; }
    }

    for(int i = 0; i < SLOTS; i++){ my_free(ptrs[i]); }
    CHECK(ok, "random: no overlap, every pointer aligned, heap valid after every operation");
    CHECK(my_heap_check(), "random: heap healthy after freeing everything");
}

int main(void){
    test_basic();
    test_reuse();
    test_coalesce();
    test_bins();
    test_alignment();
    test_block_sizes();
    test_big();
    test_grow();
    test_bad_free();
    test_random();

    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
