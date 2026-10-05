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


//calloc must return zeroed memory EVEN when the block was dirty before (reused), and refuse overflow
static void test_calloc(void){
    my_heap_init();

    //dirty a block, free it, then calloc the same size: we should get the same spot, cleared
    unsigned char* dirty = my_malloc(500);
    memset(dirty, 0xFF, 500);
    my_free(dirty);

    unsigned char* z = my_calloc(100, 5);
    int zero = 1;
    for(int i = 0; i < 500; i++){ if(z[i] != 0) zero = 0; }
    CHECK(z == dirty, "calloc: reused the dirty block");
    CHECK(zero, "calloc: reused memory is zeroed");
    my_free(z);

    //count * size that wraps around SIZE_MAX must fail instead of returning a tiny block
    CHECK(my_calloc((size_t)1 << 63, 4) == NULL, "calloc: overflow returns NULL");
    CHECK(my_calloc(SIZE_MAX, SIZE_MAX) == NULL, "calloc: huge overflow returns NULL");
    CHECK(my_calloc(0, 10) == NULL, "calloc: zero count returns NULL (same as malloc(0))");

    //a big (mmap'd) calloc is zero too
    unsigned char* big = my_calloc(1000, 1000);
    int big_zero = 1;
    for(size_t i = 0; i < 1000000; i += 997){ if(big[i] != 0) big_zero = 0; }
    CHECK(big && big_zero && big[999999] == 0, "calloc: large block is zeroed");
    my_free(big);
    CHECK(my_heap_check(), "calloc: heap healthy");
}

//the basic rules of realloc
static void test_realloc_basic(void){
    my_heap_init();
    my_heap_stats_t s;

    //realloc(NULL, n) is malloc(n)
    unsigned char* p = my_realloc(NULL, 100);
    CHECK(p != NULL && (uintptr_t)p % MY_ALLOC_ALIGNMENT == 0, "realloc: NULL acts like malloc");
    for(int i = 0; i < 100; i++){ p[i] = (unsigned char)i; }

    //a request that fits in the block we already have must not move it
    unsigned char* same = my_realloc(p, 100);
    CHECK(same == p, "realloc: same size keeps the pointer");

    //growing keeps the old contents (whether it moved or not)
    unsigned char* grown = my_realloc(p, 5000);
    int ok = (grown != NULL);
    for(int i = 0; ok && i < 100; i++){ if(grown[i] != (unsigned char)i) ok = 0; }
    CHECK(ok, "realloc: growing keeps the old bytes");
    memset(grown, 7, 5000);
    CHECK(my_heap_check(), "realloc: heap healthy after growing");

    //shrinking keeps the beginning
    for(int i = 0; i < 40; i++){ grown[i] = (unsigned char)(i + 1); }
    unsigned char* shrunk = my_realloc(grown, 40);
    ok = (shrunk != NULL);
    for(int i = 0; ok && i < 40; i++){ if(shrunk[i] != (unsigned char)(i + 1)) ok = 0; }
    CHECK(ok, "realloc: shrinking keeps the first bytes");
    CHECK(my_heap_check(), "realloc: heap healthy after shrinking");

    //realloc(p, 0) frees the block and returns NULL
    CHECK(my_realloc(shrunk, 0) == NULL, "realloc: size 0 returns NULL");
    my_heap_stats(&s);
    CHECK(s.used_blocks == 0, "realloc: size 0 freed the block");

    //a request that cannot be satisfied leaves the old block valid and untouched
    unsigned char* keep = my_malloc(64);
    memset(keep, 0x5A, 64);
    CHECK(my_realloc(keep, SIZE_MAX / 2 + 1) == NULL, "realloc: impossible size returns NULL");
    CHECK(keep[0] == 0x5A && keep[63] == 0x5A, "realloc: failed realloc leaves the old block alone");
    my_free(keep);
    CHECK(my_heap_check(), "realloc: heap healthy at the end");
}

//realloc should resize WHERE THE BLOCK IS when the neighbour allows it
static void test_realloc_in_place(void){
    my_heap_init();

    //layout: [a][b][c]. c stops b from merging with the big free space at the end
    unsigned char* a = my_malloc(100);
    unsigned char* b = my_malloc(300);
    unsigned char* c = my_malloc(100);
    memset(a, 1, 100);

    //b is in use right after a, so a cannot grow in place and has to move
    unsigned char* moved = my_realloc(a, 400);
    CHECK(moved != a && moved[0] == 1 && moved[99] == 1, "in place: blocked by a neighbour, so it moves and keeps data");

    //the old spot of a is free now, and b follows it. free b: a's old spot + b merge into one free block
    my_free(b);

    //c shrinks: the freed tail merges with the free space after c
    my_free(moved);
    unsigned char* d = my_malloc(200);
    unsigned char* e = my_malloc(100);   //keeps d from touching the end of the chunk
    memset(d, 2, 200);

    unsigned char* d2 = my_realloc(d, 60);
    CHECK(d2 == d, "in place: shrinking keeps the same pointer");
    CHECK(my_heap_check(), "in place: heap healthy after shrinking (tail merged or split off)");

    //now there is free space right after d2, so growing back must not move it
    unsigned char* d3 = my_realloc(d2, 200);
    CHECK(d3 == d, "in place: growing into the free tail keeps the same pointer");
    CHECK(d3[0] == 2 && d3[59] == 2, "in place: data survives");
    CHECK(my_heap_check(), "in place: heap healthy after growing back");

    my_free(d3); my_free(e); my_free(c);
    CHECK(my_heap_check(), "in place: heap healthy at the end");
}

//big blocks are resized with mremap, they must keep their data and their accounting
static void test_realloc_large(void){
    my_heap_init();
    my_heap_stats_t s;

    size_t size = 400000;
    unsigned char* p = my_malloc(size);
    for(size_t i = 0; i < size; i++){ p[i] = (unsigned char)(i % 251); }

    //grow a lot
    unsigned char* q = my_realloc(p, 5000000);
    int ok = (q != NULL && (uintptr_t)q % MY_ALLOC_ALIGNMENT == 0);
    for(size_t i = 0; ok && i < size; i++){ if(q[i] != (unsigned char)(i % 251)) ok = 0; }
    CHECK(ok, "large realloc: growing keeps the data and the alignment");
    q[4999999] = 9;

    my_heap_stats(&s);
    CHECK(s.mapped_bytes >= 5000000 && s.mapped_bytes < 5200000, "large realloc: accounting follows the new size");

    //shrink, but stay big
    unsigned char* r = my_realloc(q, 200000);
    ok = (r != NULL);
    for(size_t i = 0; ok && i < 200000; i++){ if(r[i] != (unsigned char)(i % 251)) ok = 0; }
    CHECK(ok, "large realloc: shrinking keeps the data");

    //shrink to something small: it should move into an arena
    unsigned char* small = my_realloc(r, 100);
    ok = (small != NULL);
    for(size_t i = 0; ok && i < 100; i++){ if(small[i] != (unsigned char)(i % 251)) ok = 0; }
    CHECK(ok, "large realloc: shrinking to a small size keeps the data");
    my_heap_stats(&s);
    CHECK(s.mapped_bytes < 2000000 && s.used_blocks == 1, "large realloc: the big mapping was given back");

    my_free(small);
    CHECK(my_heap_check(), "large realloc: heap healthy");
}

//memalign for alignments bigger than what blocks normally have
static void test_memalign(void){
    my_heap_init();
    my_heap_stats_t s;

    size_t alignments[] = {16, 32, 64, 128, 256, 4096, 65536, 1 << 20};
    int ok = 1;
    void* ptrs[8];
    for(int i = 0; i < 8; i++){
        ptrs[i] = my_memalign(alignments[i], 1000);
        if(ptrs[i] == NULL || (uintptr_t)ptrs[i] % alignments[i] != 0){ ok = 0; continue; }
        memset(ptrs[i], i + 1, 1000);          //the whole requested size must be usable
        if(my_usable_size(ptrs[i]) < 1000) ok = 0;
    }
    CHECK(ok, "memalign: every alignment from 16 to 1 MB is honoured and usable");

    for(int i = 0; i < 8; i++){
        if(((unsigned char*)ptrs[i])[999] != (unsigned char)(i + 1)) ok = 0;
    }
    CHECK(ok, "memalign: blocks do not overlap");

    CHECK(my_memalign(0, 10) == NULL, "memalign: alignment 0 rejected");
    CHECK(my_memalign(48, 10) == NULL, "memalign: non power of two rejected");
    CHECK(my_memalign(4096, 0) == NULL, "memalign: size 0 returns NULL");
    CHECK(my_memalign(4096, SIZE_MAX) == NULL, "memalign: absurd size returns NULL");

    //over-aligned blocks can be reallocated too (they move, keeping their data)
    unsigned char* a = my_memalign(4096, 100);
    memset(a, 0xAB, 100);
    unsigned char* b = my_realloc(a, 10000);
    CHECK(b != NULL && b[0] == 0xAB && b[99] == 0xAB, "memalign: realloc keeps the data");
    my_free(b);

    for(int i = 0; i < 8; i++){ my_free(ptrs[i]); }
    //the 16..64 byte alignments came from the arena (its one 1 MB chunk stays mapped, by design),
    //everything bigger had its own mapping, and all of those must be gone
    my_heap_stats(&s);
    CHECK(s.used_blocks == 0 && s.mapped_bytes == (size_t)(1 << 20), "memalign: freeing returns every over-aligned mapping to the OS");
    CHECK(my_heap_check(), "memalign: heap healthy");
}

//usable_size reports at least what was asked for, and 0 for nothing / already freed
static void test_usable_size(void){
    my_heap_init();
    CHECK(my_usable_size(NULL) == 0, "usable size: NULL is 0");

    int ok = 1;
    for(size_t n = 1; n < 3000; n += 37){
        void* p = my_malloc(n);
        if(my_usable_size(p) < n){ ok = 0; }
        my_free(p);
    }
    CHECK(ok, "usable size: always at least the requested size");

    void* big = my_malloc(500000);
    CHECK(my_usable_size(big) >= 500000, "usable size: large block");
    my_free(big);
}

//random malloc / calloc / realloc / free with a pattern in every block, heap checked every step
#define RSLOTS 200
static void test_random_realloc(void){
    my_heap_init();
    srand(777);

    void* ptrs[RSLOTS] = {0};
    size_t sizes[RSLOTS] = {0};
    unsigned char tags[RSLOTS] = {0};
    int ok = 1;

    for(int i = 0; i < 40000 && ok; i++){
        int slot = rand() % RSLOTS;
        int op = rand() % 4;
        size_t size = (rand() % 15 == 0) ? (size_t)(rand() % 300000 + 1) : (size_t)(rand() % 700 + 1);

        if(ptrs[slot] == NULL){
            //new block, half the time through calloc (which must also come back zeroed)
            void* p = (op < 2) ? my_calloc(1, size) : my_malloc(size);
            if(p == NULL || (uintptr_t)p % MY_ALLOC_ALIGNMENT != 0){ ok = 0; break; }
            if(op < 2){
                unsigned char* bytes = p;
                for(size_t j = 0; j < size; j += 13){ if(bytes[j] != 0){ ok = 0; } }
            }
            tags[slot] = (unsigned char)(rand() % 255 + 1);
            memset(p, tags[slot], size);
            ptrs[slot] = p;
            sizes[slot] = size;
        }
        else if(op == 0){
            //free (after checking nobody scribbled on it)
            unsigned char* bytes = ptrs[slot];
            for(size_t j = 0; j < sizes[slot]; j++){ if(bytes[j] != tags[slot]){ ok = 0; break; } }
            my_free(ptrs[slot]);
            ptrs[slot] = NULL;
        }
        else{
            //resize: the first min(old, new) bytes must survive
            void* p = my_realloc(ptrs[slot], size);
            if(p == NULL || (uintptr_t)p % MY_ALLOC_ALIGNMENT != 0){ ok = 0; break; }
            size_t keep = sizes[slot] < size ? sizes[slot] : size;
            unsigned char* bytes = p;
            for(size_t j = 0; j < keep; j++){ if(bytes[j] != tags[slot]){ ok = 0; break; } }
            memset(p, tags[slot], size);
            ptrs[slot] = p;
            sizes[slot] = size;
        }

        if(!my_heap_check()){ ok = 0; }
    }

    for(int i = 0; i < RSLOTS; i++){ my_free(ptrs[i]); }
    CHECK(ok, "random realloc: data preserved, aligned, heap valid after every operation");
    my_heap_stats_t s;
    my_heap_stats(&s);
    CHECK(s.used_blocks == 0 && my_heap_check(), "random realloc: nothing left in use, heap healthy");
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
    test_calloc();
    test_realloc_basic();
    test_realloc_in_place();
    test_realloc_large();
    test_memalign();
    test_usable_size();
    test_random_realloc();

    printf("\n%s (%d failures)\n", failures == 0 ? "ALL TESTS PASSED" : "SOME TESTS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
