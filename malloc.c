#define _GNU_SOURCE   /* mremap */
#include <unistd.h>    /* sysconf, write */
#include <string.h>    /* memcpy, memset */
#include <stdint.h>    /* uintptr_t, SIZE_MAX */
#include <pthread.h>   /* pthread_mutex_t */
#include <sys/mman.h>  /* mmap, munmap */
#include "include/malloc.h"


/*
 * HOW THE HEAP IS LAID OUT (read this first!)
 * -------------------------------------------
 * Memory is cut up into "blocks". Every block looks like this:
 *
 *      | header | ........ payload ........ | footer |
 *      ^        ^
 *      |        +-- this is the pointer we hand back to the user
 *      +-- start of the block
 *
 *  - header : size of the payload, whether the block is free, which arena owns
 *             it, and "next"/"prev" pointers used only while it sits in a free list
 *  - footer : a copy of the payload size. It lets us look BACKWARDS from a
 *             block to find the block before it (needed for coalescing)
 *
 * Because every block knows its size, we can jump from one block to the one
 * right after it:   next block = this block + header + size + footer
 *
 * Blocks live inside "chunks": big pieces of memory we get from the OS.
 * Every chunk has fence markers at both ends so we never walk off the edge:
 *
 *      | pad | prologue | block | block | block ... | epilogue |
 *
 *  - pad      : a few unused bytes so the first header lands on the right offset
 *  - prologue : a footer with size 0 ("there is no block before this").
 *               It also remembers how long the whole chunk is.
 *  - epilogue : a header with size 0 that is never free ("no block after").
 *               Its "next" pointer says where this arena's next chunk starts.
 *
 *
 * ARENAS (Stage 4): ONE HEAP PER THREAD
 * -------------------------------------
 * In Stage 2 every thread shared one heap behind one lock, so threads spent
 * their time waiting in line. Now the allocator owns a table of ARENAS. An
 * arena is a complete little heap: its own lock, its own free lists, its own
 * chunks. Each thread is given one arena the first time it allocates, so
 * threads almost never touch each other's data or locks.
 *
 *      thread 1 --> arena 0 (lock 0, bins, chunks)
 *      thread 2 --> arena 1 (lock 1, bins, chunks)
 *      thread 3 --> arena 2 (lock 2, bins, chunks)
 *
 * What about a block that thread 1 allocated but thread 2 frees? Every
 * header stores the id of the arena that owns the block. free() reads that
 * id and locks THAT arena (not the freeing thread's own), puts the block back
 * in the owner's bins, and unlocks. It is simple and always correct. The cost:
 * a cross-thread free briefly takes someone else's lock, so a thread that
 * mostly frees other threads' memory still causes some contention.
 *
 * Arenas get memory with mmap() instead of sbrk(). sbrk has ONE program break
 * for the whole process, so it would be another shared bottleneck (and it
 * collides with the real malloc). mmap hands out independent regions, so
 * arenas never fight over anything.
 *
 * Very large requests (>= LARGE_THRESHOLD) skip the arenas: each gets its own
 * mmap and goes straight back to the OS with munmap on free.
 *
 *
 * ALIGNMENT (Stage 5): EVERY PAYLOAD STARTS ON A 64 BYTE BOUNDARY
 * ----------------------------------------------------------------
 * 64 bytes is the size of one CPU cache line, and of one AVX-512 vector (an
 * AVX2 vector is 32 bytes). Data that starts on a 64 byte boundary can be loaded
 * with aligned vector instructions and never has its first 64 bytes split across
 * two cache lines. Real malloc only promises 16.
 *
 * The header is 32 bytes and the footer is 16, so to make the PAYLOAD land on a
 * boundary we place things like this (addresses mod 64):
 *
 *      ... footer | header | payload ........ | footer | header | payload ...
 *                 ^ 32     ^ 0 (aligned!)              ^ 32     ^ 0
 *
 *  - every block's total size (header + payload + footer) is a multiple of 64,
 *    so if one payload is aligned the next one is too
 *  - the first header of a chunk is placed at offset 32 (CHUNK_LEAD pads to get there)
 *  - payload sizes therefore come in steps of 64 offset by -48: 16, 80, 144, 208...
 *
 * The price is memory: a request is rounded up to the next such step, so on
 * average a block wastes a bit more than before (~24 bytes more per block than 16 byte
 * alignment). Build with -DMY_ALLOC_ALIGNMENT=16 to see the difference. Note that
 * alignment only promises where the payload STARTS: the last cache line of a small
 * payload still shares its line with the next block's header, so this is not a
 * complete false-sharing cure on its own (per-thread arenas help, because neighbours
 * usually belong to the same thread).
 *
 *
 * FREE LISTS ARE SPLIT BY SIZE (Stage 3)
 * --------------------------------------
 * Each arena keeps many free lists ("bins"), one per power-of-two size range:
 *
 *      bin 0:  payload 16  .. 31        bin 3:  payload 128 .. 255
 *      bin 1:  payload 32  .. 63        bin 4:  payload 256 .. 511
 *      bin 2:  payload 64  .. 127       ...and so on, each bin doubles
 *
 * To malloc(size):
 *   1. look in the bin for that size and take the first block that fits
 *   2. otherwise take the FIRST block of any bigger bin (always big enough).
 *      A bitmap (one bit per bin, 1 = "has blocks") finds that bin instantly.
 *   3. otherwise ask the OS for another chunk.
 * Free lists are doubly linked so any block can be unlinked in constant time.
 *
 *
 * LOCKING RULES
 * -------------
 *  - Each arena has one mutex protecting everything inside that arena.
 *  - Only the public functions at the bottom take locks. Everything else
 *    assumes the caller already holds the right arena's lock, so we never
 *    lock twice (which would deadlock).
 *  - No function ever holds two arena locks at once, so arenas can't deadlock
 *    each other.
 *  - Build with -DMY_ALLOC_NO_LOCK to remove the arena locks. Never ship that:
 *    it is only there so ThreadSanitizer can show the races the locks prevent
 *    (with per-thread arenas they only show up on cross-thread frees).
 *
 *
 * REALLOC, CALLOC AND FRIENDS (Stage 7)
 * -------------------------------------
 *  - realloc tries to resize IN PLACE first: shrinking splits off the tail (and merges it
 *    with a free neighbour), growing swallows a free block that sits right after us.
 *    Only if that is impossible do we malloc + copy + free. Big mmap'd blocks use mremap,
 *    which lets the kernel move the pages without copying any bytes.
 *  - calloc checks n * size for overflow, and skips the memset for freshly mmap'd blocks
 *    (the OS already hands those out zeroed).
 *  - memalign / posix_memalign / aligned_alloc: anything up to ALIGNMENT is a normal block.
 *    Bigger alignments get their own mmap, with a header placed right before the aligned
 *    payload that remembers where the mapping really starts (arena_id LARGE_ALIGNED_ID).
 *
 *
 * RUNNING INSIDE OTHER PROGRAMS (LD_PRELOAD)
 * ------------------------------------------
 * preload.c exports the standard names (malloc, free, ...) so this file can be built into
 * libmyalloc.so and swapped in under a real program. That puts extra rules on this file:
 *  - we may never call anything that calls malloc (printf, fopen, ...) because malloc IS us.
 *    Error messages therefore use write(2) directly.
 *  - the per-thread arena pointer uses the "initial-exec" TLS model, so reading it never
 *    triggers the dynamic loader (which allocates memory).
 *  - fork(): another thread might hold an arena lock at the moment of the fork, and the child
 *    would wait for it forever. pthread_atfork handlers take every lock before the fork and
 *    release them in both parent and child.
 *  - build with -DMY_ALLOC_SINGLE_ARENA to put every thread in ONE arena. That is the
 *    "one global lock" design built from the same code, used as the benchmark baseline.
 *
 * Known limitations (also good things to say out loud in an interview):
 *  - chunks are never returned to the OS, they are only reused
 *  - if a thread's arena is out of memory we fail, we don't borrow from others
 *  - threads are spread over MAX_ARENAS arenas round-robin; past that they share
 *  - an arena is not recycled when its thread exits
 *  - double-freeing a LARGE block is not detected (the memory is already unmapped)
 *  - an over-aligned request (alignment > ALIGNMENT) costs at least one whole page
 *  - freeing a pointer that did not come from us is only caught by cheap checks (alignment and
 *    the arena id), so it may still crash. real programs do not do this
 */


//initialize vairables

//how much we ask the OS for each time an arena runs out of room (mmap only commits
//pages when they are touched, so a big chunk costs nothing until it is used)
#define MIN_EXTEND_SIZE (1 << 20)

//requests this big or bigger get their own mmap instead of living in an arena
#define LARGE_THRESHOLD (128 * 1024)

//size of the arena table. threads past this number share arenas
#define MAX_ARENAS (64)

//arena_id used by blocks that were mmap'd on their own
#define LARGE_ARENA_ID (-1)

//arena_id used by mmap'd blocks that were asked for with a big alignment (see large_aligned_alloc)
#define LARGE_ALIGNED_ID (-2)

//TEST ONLY: -DMY_ALLOC_TEST_SLOW_LOCK makes every thread linger inside its critical section, so
//tests can reliably make a fork() land while a lock is held (otherwise the window is far too small
//to hit). never use it for real
#ifdef MY_ALLOC_TEST_SLOW_LOCK
#define LINGER() usleep(30)
#else
#define LINGER() ((void)0)
#endif

//with -DMY_ALLOC_SINGLE_ARENA every thread shares arena 0 (the one-global-lock baseline)
#ifdef MY_ALLOC_SINGLE_ARENA
#define ARENA_LIMIT (1)
#else
#define ARENA_LIMIT (MAX_ARENAS)
#endif

//every payload address we return is a multiple of this. 64 = one cache line (see the ALIGNMENT
//section at the top). real malloc uses 16. can be changed at build time: -DMY_ALLOC_ALIGNMENT=16
#define ALIGNMENT (MY_ALLOC_ALIGNMENT)

//how many size bins we have. bin 0 is payload 16..31, each next bin doubles,
//and the last bin catches everything bigger
#define NUM_BINS (32)

//log2(16) = 4. subtracting this makes the 16 byte bin come out as bin 0
#define BIN_SHIFT (4)




/* INITIALIZE THE HEADERS */

//header
//(aligned(16) pads the struct to 32 bytes so the payload after it is 16 byte aligned)
typedef struct block_header {
    //holds size (of the payload only, not counting header or footer)
    size_t size;
    //is it free
    int is_free;
    //which arena owns this block (or LARGE_ARENA_ID). sits in what used to be padding, so it is free
    int arena_id;
    //pointer to next free block in the same bin
    struct block_header* next;
    //pointer to the previous free block in the same bin (lets us unlink in O(1))
    struct block_header* prev;
} __attribute__((aligned(16))) header;

//footer
//(16 bytes so the next header starts on a 16 byte boundary)
typedef struct block_footer {
    //hold size
    size_t size;
    //only used by the prologue fence: how many bytes long the whole chunk is
    size_t chunk_len;
} __attribute__((aligned(16))) footer;

//total bytes of bookkeeping every block costs
#define OVERHEAD (sizeof(header) + sizeof(footer))

/* ALIGNMENT MATH. all of these are worked out from ALIGNMENT so any power of two (16, 32, 64, 128...) works */

//a header must start this many bytes past a multiple of ALIGNMENT, so that the payload
//32 bytes after it lands exactly ON a multiple. (ALIGNMENT 64 -> 32,  ALIGNMENT 16 -> 0)
#define HEADER_OFFSET ((ALIGNMENT - sizeof(header) % ALIGNMENT) % ALIGNMENT)

//unused bytes at the very start of a chunk so the first block's header lands on HEADER_OFFSET.
//(chunk start is page aligned, then: pad, 16 byte prologue, then the first header)
#define CHUNK_LEAD ((HEADER_OFFSET + ALIGNMENT - sizeof(footer)) % ALIGNMENT)

//everything in a chunk that is not a block: the pad, the prologue and the epilogue
#define CHUNK_FENCES (CHUNK_LEAD + sizeof(footer) + sizeof(header))

//smallest payload a block is allowed to have: the smallest valid size that is at least 1.
//(ALIGNMENT 64 -> 16.  ALIGNMENT 128 -> 80.) also guarantees size 0 is only ever a fence
#define MIN_PAYLOAD (((OVERHEAD + 1 + ALIGNMENT - 1) / ALIGNMENT) * ALIGNMENT - OVERHEAD)

//compile-time sanity checks, so a bad MY_ALLOC_ALIGNMENT fails the build instead of corrupting memory
_Static_assert(sizeof(header) == 32, "header must stay 32 bytes");
_Static_assert(sizeof(footer) == 16, "footer must stay 16 bytes");
_Static_assert(ALIGNMENT >= 16 && (ALIGNMENT & (ALIGNMENT - 1)) == 0, "alignment must be a power of two, at least 16");
_Static_assert(ALIGNMENT <= 1024, "alignment must be at most 1024");
_Static_assert(CHUNK_FENCES % ALIGNMENT == 0, "chunk fences must keep blocks aligned");

//arena
//one complete heap. aligned(64) puts each arena on its own cache line so two threads
//working in different arenas never slow each other down by sharing a cache line (false sharing)
typedef struct arena {
    //protects everything in this arena
    pthread_mutex_t lock;
    //one free list per size range. bins[i] points to the first free block in bin i (or NULL)
    header* bins[NUM_BINS];
    //bit i is 1 when bins[i] has at least one block. lets us find a non-empty bin without looping
    unsigned long bin_bitmap;
    //first block of the first chunk (where heap walking starts)
    header* first_block;
    //epilogue at the very end of the newest chunk (so a new chunk can be chained onto it)
    header* last_epilogue;
    //total bytes of chunks this arena has mapped
    size_t mapped_bytes;
    //position in the arena table
    int id;
    //has this arena's lock been set up yet. read/written ONLY through arena_ready / the atomic
    //store in get_thread_arena, because many threads look at it without holding any lock
    int initialized;
} __attribute__((aligned(64))) arena_t;

/* INITIALIZE THE GLOBAL VAIRIABLES*/

//the table of arenas (starts all zero)
static arena_t arenas[MAX_ARENAS];

//is this arena set up? "acquire" means: if we see 1 here, we also see everything the
//thread that set it up wrote before (its lock and id). pairs with the "release" store below
static int arena_ready(arena_t* a){
    return __atomic_load_n(&a -> initialized, __ATOMIC_ACQUIRE);
}

//only used while handing a new thread its arena (once per thread, never on the fast path)
static pthread_mutex_t arena_table_lock = PTHREAD_MUTEX_INITIALIZER;
static int next_arena_id = 0;

//each thread remembers its own arena. __thread gives every thread its own copy of this variable
//initial-exec: the variable lives at a fixed spot reachable without calling into the dynamic
//loader, which matters when we are preloaded (the loader would call malloc, which is us)
static __thread arena_t* thread_arena __attribute__((tls_model("initial-exec"))) = NULL;

//bytes currently held by large (directly mmap'd) blocks. updated with atomics, no lock
static size_t large_mapped_bytes = 0;

#ifdef MY_ALLOC_NO_LOCK
#define LOCK_ARENA(a)   ((void)0)
#define UNLOCK_ARENA(a) ((void)0)
#else
#define LOCK_ARENA(a)   pthread_mutex_lock(&(a) -> lock)
#define UNLOCK_ARENA(a) pthread_mutex_unlock(&(a) -> lock)
#endif


/* INITIALIZE THE HELPER FUNCTIONS*/

//copy a string onto the end of buf (never past cap)
static void append_text(char* buf, size_t* len, size_t cap, const char* text){
    while(*text != '\0' && *len < cap){
        buf[*len] = *text;
        (*len)++;
        text++;
    }
}

//print "<who><message> (at 0x...)" to stderr WITHOUT stdio. printf/fprintf can call malloc, and
//when we are preloaded under a real program malloc is us, so that would recurse forever
static void log_error(const char* who, const char* message, void* where){
    char buf[200];
    size_t len = 0;
    append_text(buf, &len, sizeof(buf) - 24, who);
    append_text(buf, &len, sizeof(buf) - 24, message);
    append_text(buf, &len, sizeof(buf) - 24, " (at 0x");

    //write the address as hex digits, most significant first
    char digits[17];
    int count = 0;
    uintptr_t value = (uintptr_t)where;
    do{
        digits[count++] = "0123456789abcdef"[value & 0xF];
        value >>= 4;
    } while(value != 0);
    while(count > 0){
        buf[len++] = digits[--count];
    }
    buf[len++] = ')';
    buf[len++] = '\n';

    //nothing sensible to do if the write fails, we are already reporting an error
    ssize_t ignored = write(2, buf, len);
    (void)ignored;
}

//turn what the user asked for into the payload size we actually use.
//the WHOLE block (header + payload + footer) has to be a multiple of ALIGNMENT, so we round
//size + OVERHEAD up and then take the overhead back off.
//ex with ALIGNMENT 64:  1 -> 16,  16 -> 16,  17 -> 80,  80 -> 80,  81 -> 144
static size_t payload_size_for(size_t size){
    size_t total = (size + OVERHEAD + ALIGNMENT - 1) & ~(size_t)(ALIGNMENT - 1);
    return total - OVERHEAD;
}

//write the footer of a block so it matches the block's header size
static void set_footer(header* block){
    //footer sits right after the payload, cast inside to char* to do math and then cast to footer
    footer* block_footer = (footer*)((char*)block + sizeof(header) + block->size);
    block_footer -> size = block -> size;
}

//get the block that sits directly after this one in memory
static header* next_block(header* block){
    return (header*)((char*)block + sizeof(header) + block -> size + sizeof(footer));
}

//which bin does a block of this payload size belong in?
//bin = floor(log2(size)), shifted so that 16 is bin 0. ex: 16->0, 32->1, 100->2, 300->4
static int bin_index(size_t size){
    //63 minus the number of leading zero bits is the position of the top 1 bit = floor(log2(size))
    int log2_size = 63 - __builtin_clzl(size);
    int index = log2_size - BIN_SHIFT;

    //very big blocks all share the last bin
    if(index >= NUM_BINS){
        index = NUM_BINS - 1;
    }
    return index;
}

//put a free block at the front of the bin (in arena a) that matches its size
static void add_to_free_list(arena_t* a, header* head){
    int bin = bin_index(head -> size);

    //point node to where the bin points, it has nothing in front of it
    head -> next = a -> bins[bin];
    head -> prev = NULL;

    //the old first block now has us in front of it
    if(a -> bins[bin] != NULL){
        a -> bins[bin] -> prev = head;
    }

    //point head of the bin here
    a -> bins[bin] = head;

    //mark the bin as non-empty
    a -> bin_bitmap |= (1UL << bin);
}

//take a specific block out of its bin (used when we merge or hand out a block).
//IMPORTANT: call this BEFORE changing block -> size, because the size decides which bin it is in
static void remove_from_free_list(arena_t* a, header* target){
    int bin = bin_index(target -> size);

    //the block in front of us skips over us
    if(target -> prev != NULL){
        target -> prev -> next = target -> next;
    }
    //no block in front means we were the head of the bin, so the bin now starts after us
    else{
        a -> bins[bin] = target -> next;
    }

    //the block behind us points back past us
    if(target -> next != NULL){
        target -> next -> prev = target -> prev;
    }

    //if the bin is now empty, clear its bit in the bitmap
    if(a -> bins[bin] == NULL){
        a -> bin_bitmap &= ~(1UL << bin);
    }

    target -> next = NULL;
    target -> prev = NULL;
}

//merge a free block with any free neighbors. The block must NOT be in a free list yet.
//returns the (possibly bigger) block, still not in a list, so the caller adds it
static header* coalesce(arena_t* a, header* block){

    //look at the block AFTER us. (the epilogue is never free so this is safe at the edge)
    header* after = next_block(block);
    if(after -> is_free == 1){
        //pull it out of its bin since it is about to disappear into us
        remove_from_free_list(a, after);

        //we grow by everything the other block owned, including its header and footer
        block -> size = block -> size + OVERHEAD + after -> size;
        set_footer(block);
    }

    //look at the block BEFORE us by reading the footer right before our header
    footer* before_footer = (footer*)((char*)block - sizeof(footer));

    //size 0 means its the prologue fence, so nothing is before us
    if(before_footer -> size != 0){
        //use the footer size to jump backwards to the start of the previous block
        header* before = (header*)((char*)before_footer - before_footer -> size - sizeof(header));

        if(before -> is_free == 1){
            remove_from_free_list(a, before);

            //the previous block swallows us
            before -> size = before -> size + OVERHEAD + block -> size;
            set_footer(before);
            block = before;
        }
    }

    return block;
}

//asks the OS (with mmap) for a new chunk for arena a, turns it into one big free block,
//and returns that block. size is the payload we need (already aligned)
static header* extend_heap(arena_t* a, size_t size){

    //add the header and footer size to size, plus the chunk's fences
    size_t request = size + OVERHEAD + CHUNK_FENCES;

    //see what kind of size we need to extend
    if (request < MIN_EXTEND_SIZE){
        request = MIN_EXTEND_SIZE;
    }

    //mmap only works in whole pages, so round up
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    request = (request + page - 1) & ~(page - 1);

    //call mmap for brand new zeroed memory that nobody else owns
    void* raw = mmap(NULL, request, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    //check if it failed
    if(raw == MAP_FAILED){
        return NULL;
    }

    //the prologue fence (a footer of size 0) goes after the pad, and remembers the chunk length
    footer* prologue = (footer*)((char*)raw + CHUNK_LEAD);
    prologue -> size = 0;
    prologue -> chunk_len = request;

    //the block comes right after the prologue and fills everything except the epilogue.
    //(this lands the header on HEADER_OFFSET, so its payload is aligned)
    header* new_block = (header*)((char*)prologue + sizeof(footer));

    //create a header
    new_block -> size = request - CHUNK_FENCES - OVERHEAD;
    new_block -> is_free = 1;
    new_block -> arena_id = a -> id;
    new_block -> next = NULL;
    new_block -> prev = NULL;

    //create a footer
    set_footer(new_block);

    //create the epilogue fence right after the block
    header* new_epilogue = next_block(new_block);
    new_epilogue -> size = 0;
    new_epilogue -> is_free = 0;
    new_epilogue -> arena_id = a -> id;
    new_epilogue -> next = NULL;
    new_epilogue -> prev = NULL;

    //first chunk ever for this arena? remember where heap walking starts
    if(a -> first_block == NULL){
        a -> first_block = new_block;
    }
    //otherwise chain the old epilogue to this chunk so heap_check can hop to it
    else{
        a -> last_epilogue -> next = new_block;
    }
    a -> last_epilogue = new_epilogue;
    a -> mapped_bytes += request;

    //push the header into the free list
    add_to_free_list(a, new_block);

    return new_block;
}

//takes a free block that is big enough and gives it to the user.
//if there is a lot left over, the leftover becomes its own free block (splitting)
static void* place_block(arena_t* a, header* block, size_t size){

    //it is no longer free, so it must leave the free list
    remove_from_free_list(a, block);

    //how much would be left over
    size_t leftover = block -> size - size;

    //only split if the leftover can hold its own header + footer + a minimum payload
    if(leftover >= OVERHEAD + MIN_PAYLOAD){
        //shrink this block to exactly what was asked for
        block -> size = size;
        set_footer(block);

        //build a new free block out of the leftover space, right after this one
        header* remainder = next_block(block);
        remainder -> size = leftover - OVERHEAD;
        remainder -> is_free = 1;
        remainder -> arena_id = a -> id;
        set_footer(remainder);

        //the leftover is free, so it goes back in the list. (its neighbors are not free,
        //because our block used to be free and free blocks are never side by side)
        add_to_free_list(a, remainder);
    }
    //otherwise the user gets the whole block, a few extra bytes are better than a useless sliver

    //set the is_free to 0
    block -> is_free = 0;

    //return the payload (right after the header)
    return (char*)block + sizeof(header);
}

//gives the calling thread its arena the first time, and remembers it after that
static arena_t* get_thread_arena(void){

    //fast path: this thread already has one, no locking at all
    if(thread_arena != NULL){
        return thread_arena;
    }

    //slow path (once per thread): pick the next arena in round-robin order
    pthread_mutex_lock(&arena_table_lock);

    int id = next_arena_id % ARENA_LIMIT;
    next_arena_id++;

    arena_t* a = &arenas[id];
    if(arena_ready(a) == 0){
        pthread_mutex_init(&a -> lock, NULL);
        a -> id = id;
        //"release" publishes the lock and id above to any thread that later sees initialized == 1
        __atomic_store_n(&a -> initialized, 1, __ATOMIC_RELEASE);
    }

    pthread_mutex_unlock(&arena_table_lock);

    thread_arena = a;
    return a;
}



/* ARENA FUNCTIONS (these assume the caller already holds the arena's lock) */

static void* malloc_unlocked(arena_t* a, size_t size){

    //figure out which bin a block of this size lives in
    int bin = bin_index(size);

    //STEP 1: walk the bin for our size and take the first block that fits (first fit).
    //blocks in this bin range from 2^k to 2^(k+1)-1, so some may be a little too small
    header* current = a -> bins[bin];
    while(current != NULL){
        //check if its free and if size is big enough
        if(current -> is_free == 1 && current -> size >= size){
            //split it if needed, mark it used and hand it over
            return place_block(a, current, size);
        }

        current = current -> next;
    }

    //STEP 2: look for ANY non-empty bin bigger than ours. (~0UL << (bin + 1)) keeps only bits above our bin.
    //every block in a bigger bin is larger than anything in our bin's range, so the first block always fits
    unsigned long bigger_bins = a -> bin_bitmap & (~0UL << (bin + 1));
    if(bigger_bins != 0){
        //count trailing zeros = index of the lowest set bit = the smallest bigger bin that has a block
        int found_bin = __builtin_ctzl(bigger_bins);
        return place_block(a, a -> bins[found_bin], size);
    }

    //STEP 3: nothing fit anywhere, so we extend
    header* new_block = extend_heap(a, size);

    //make sure its valid
    if(new_block == NULL){
        return NULL;
    }

    //the new block is big enough by construction
    return place_block(a, new_block, size);
}

static void free_unlocked(arena_t* a, header* block){

    //catch double frees instead of corrupting the free list
    if(block -> is_free == 1){
        log_error("my_free: ", "double free detected", (char*)block + sizeof(header));
        return;
    }

    //mark it free
    block -> is_free = 1;

    //merge with free neighbors
    block = coalesce(a, block);

    //put the (possibly bigger) block into the right bin
    add_to_free_list(a, block);
}


//if an allocated block has more room than "need", give the extra back as its own free block.
//(the extra may touch a free block after it, so it is merged with that one)
static void trim_block(arena_t* a, header* block, size_t need){
    size_t leftover = block -> size - need;

    //same rule as place_block: only split if the leftover can be a real block
    if(leftover >= OVERHEAD + MIN_PAYLOAD){
        block -> size = need;
        set_footer(block);

        header* remainder = next_block(block);
        remainder -> size = leftover - OVERHEAD;
        remainder -> is_free = 1;
        remainder -> arena_id = a -> id;
        set_footer(remainder);

        //merge with a free block after it (our block is in use, so nothing to merge with before it)
        remainder = coalesce(a, remainder);
        add_to_free_list(a, remainder);
    }
}

//try to resize an allocated arena block WITHOUT moving it. returns 1 on success, 0 if it won't fit.
//need is the new payload size, already rounded by payload_size_for
static int realloc_in_place(arena_t* a, header* block, size_t need){

    //growing: only possible if the block right after us is free and the two together are big enough
    if(need > block -> size){
        header* after = next_block(block);
        if(after -> is_free != 1 || block -> size + OVERHEAD + after -> size < need){
            return 0;
        }

        //swallow the free block (header, footer and all)
        remove_from_free_list(a, after);
        block -> size = block -> size + OVERHEAD + after -> size;
        set_footer(block);
    }

    //if we now have more than we need (shrinking, or swallowed a big block), hand the tail back
    trim_block(a, block, need);
    return 1;
}


/* LARGE BLOCKS (no arena, no lock: the OS does the bookkeeping) */

static void* large_alloc(size_t size){

    //a header in front of the payload (placed at HEADER_OFFSET so the payload is aligned),
    //all rounded up to whole pages
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t total = (HEADER_OFFSET + sizeof(header) + size + page - 1) & ~(page - 1);

    void* raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(raw == MAP_FAILED){
        return NULL;
    }

    header* block = (header*)((char*)raw + HEADER_OFFSET);
    block -> size = total - HEADER_OFFSET - sizeof(header);
    block -> is_free = 0;
    block -> arena_id = LARGE_ARENA_ID;
    block -> next = NULL;
    block -> prev = NULL;

    __atomic_fetch_add(&large_mapped_bytes, total, __ATOMIC_RELAXED);

    return (char*)block + sizeof(header);
}

static void large_free(header* block){
    void* base;
    size_t total;

    if(block -> arena_id == LARGE_ALIGNED_ID){
        //an over-aligned block stored where its mapping starts and how long it is
        base = (void*)block -> next;
        total = (size_t)(uintptr_t)block -> prev;
    }
    else{
        //walk back to where the mapping really starts
        base = (char*)block - HEADER_OFFSET;
        total = HEADER_OFFSET + sizeof(header) + block -> size;
    }
    __atomic_fetch_sub(&large_mapped_bytes, total, __ATOMIC_RELAXED);

    //give the pages straight back to the OS
    munmap(base, total);
}

//a block whose payload must start on a boundary bigger than ALIGNMENT (ex: 4096 for a page).
//we map extra room, pick the first aligned spot, and put the header right in front of it.
//(an allocated block never uses next/prev, so we borrow them to remember the mapping)
static void* large_aligned_alloc(size_t alignment, size_t size){
    size_t page = (size_t)sysconf(_SC_PAGESIZE);

    //worst case we skip alignment - 1 bytes to find an aligned spot, plus a header in front of it
    size_t total;
    if(__builtin_add_overflow(size, alignment + sizeof(header), &total)){
        return NULL;
    }
    if(__builtin_add_overflow(total, page - 1, &total)){
        return NULL;
    }
    total &= ~(page - 1);

    void* raw = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if(raw == MAP_FAILED){
        return NULL;
    }

    //first address at least a header past the start that is a multiple of alignment
    uintptr_t payload = ((uintptr_t)raw + sizeof(header) + alignment - 1) & ~(uintptr_t)(alignment - 1);

    header* block = (header*)(payload - sizeof(header));
    block -> size = (size_t)((uintptr_t)raw + total - payload);
    block -> is_free = 0;
    block -> arena_id = LARGE_ALIGNED_ID;
    block -> next = (header*)raw;
    block -> prev = (header*)(uintptr_t)total;

    __atomic_fetch_add(&large_mapped_bytes, total, __ATOMIC_RELAXED);

    return (void*)payload;
}

//resize a LARGE_ARENA_ID block (new payload size need is already rounded and still >= LARGE_THRESHOLD).
//mremap lets the kernel move or stretch the pages, so no bytes are copied. returns NULL on failure
//and leaves the old block alone
static void* large_realloc(header* block, size_t need){
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t old_total = HEADER_OFFSET + sizeof(header) + block -> size;
    size_t new_total = (HEADER_OFFSET + sizeof(header) + need + page - 1) & ~(page - 1);

    if(new_total == old_total){
        return (char*)block + sizeof(header);
    }

    void* new_base = mremap((char*)block - HEADER_OFFSET, old_total, new_total, MREMAP_MAYMOVE);
    if(new_base == MAP_FAILED){
        return NULL;
    }

    //the header moved with the pages, just fix its size
    header* moved = (header*)((char*)new_base + HEADER_OFFSET);
    moved -> size = new_total - HEADER_OFFSET - sizeof(header);

    if(new_total > old_total){
        __atomic_fetch_add(&large_mapped_bytes, new_total - old_total, __ATOMIC_RELAXED);
    }
    else{
        __atomic_fetch_sub(&large_mapped_bytes, old_total - new_total, __ATOMIC_RELAXED);
    }

    return (char*)moved + sizeof(header);
}


/* HEAP CHECKER (assumes the caller already holds the arena's lock) */

//print what went wrong and return 0 so the caller can do: return report(...)
static int report(const char* message, void* where){
    log_error("my_heap_check: ", message, where);
    return 0;
}

//walks ONE arena's chunks, then its bins, and makes sure everything agrees.
//returns 1 if healthy, 0 (and prints why) if not
static int check_arena(arena_t* a){

    //this arena never allocated anything, nothing to be wrong
    if(a -> first_block == NULL){
        return 1;
    }

    //how many free blocks we see while walking memory, compared to the bins at the end
    size_t free_blocks_in_heap = 0;

    //start at the first block of the first chunk
    header* block = a -> first_block;

    while(1){
        //the prologue right before this chunk's first block tells us where the chunk ends
        footer* prologue = (footer*)((char*)block - sizeof(footer));
        if(prologue -> size != 0){
            return report("chunk does not start with a prologue fence", prologue);
        }
        char* chunk_limit = ((char*)prologue - CHUNK_LEAD) + prologue -> chunk_len;

        //walk every block in this chunk
        while(1){
            //header has to sit at HEADER_OFFSET (which makes the payload aligned) and inside the chunk
            if((uintptr_t)block % ALIGNMENT != HEADER_OFFSET){
                return report("block is not aligned (payload would not be on a boundary)", block);
            }
            if((char*)block + sizeof(header) > chunk_limit){
                return report("block is outside its chunk", block);
            }
            if(block -> arena_id != a -> id){
                return report("block says it belongs to a different arena", block);
            }

            //size 0 means we hit an epilogue, which is the end of the chunk
            if(block -> size == 0){
                if(block -> is_free != 0){
                    return report("epilogue is marked free", block);
                }
                if((char*)block + sizeof(header) != chunk_limit){
                    return report("epilogue is not at the end of its chunk", block);
                }
                break;
            }

            //check the size is sane BEFORE trusting it to jump forward
            if((block -> size + OVERHEAD) % ALIGNMENT != 0 || block -> size < MIN_PAYLOAD){
                return report("block has a bad size", block);
            }
            if(block -> size > (size_t)(chunk_limit - (char*)block) ||
               (size_t)(chunk_limit - (char*)block) < OVERHEAD + block -> size + sizeof(header)){
                return report("block runs past the end of its chunk", block);
            }
            if(block -> is_free != 0 && block -> is_free != 1){
                return report("is_free is neither 0 nor 1", block);
            }

            //header and footer must agree on the size
            footer* block_footer = (footer*)((char*)block + sizeof(header) + block -> size);
            if(block_footer -> size != block -> size){
                return report("header size and footer size do not match", block);
            }

            header* after = next_block(block);

            //two free blocks side by side means coalescing missed one
            if(block -> is_free == 1){
                free_blocks_in_heap++;
                if(after -> is_free == 1){
                    return report("two free blocks are side by side", block);
                }
            }

            block = after;
        }

        //no next chunk means we have walked this whole arena
        if(block -> next == NULL){
            if(block != a -> last_epilogue){
                return report("final epilogue does not match last_epilogue", block);
            }
            break;
        }

        //otherwise hop to the next chunk
        block = block -> next;
    }

    //now walk EVERY bin and make sure the lists match what we found in memory
    size_t free_list_count = 0;

    for(int b = 0; b < NUM_BINS; b++){
        //the bitmap bit must say "non-empty" exactly when the bin has blocks
        int bit_set = (int)((a -> bin_bitmap >> b) & 1UL);
        if(bit_set != (a -> bins[b] != NULL)){
            return report("bin_bitmap is out of sync with a bin", a -> bins[b]);
        }

        header* node = a -> bins[b];
        header* previous = NULL;

        while(node != NULL){
            if(node -> is_free != 1){
                return report("free list contains a block that is not free", node);
            }
            if(node -> arena_id != a -> id){
                return report("free list contains another arena's block", node);
            }
            //a block's size decides its bin, so it must be sitting in the right one
            if(bin_index(node -> size) != b){
                return report("block is in the wrong size bin", node);
            }
            //walking backwards must agree with walking forwards
            if(node -> prev != previous){
                return report("prev pointer does not match the list order", node);
            }

            free_list_count++;

            //if the lists hold more blocks than exist in memory there is a loop or a stale entry
            if(free_list_count > free_blocks_in_heap){
                return report("free lists have more entries than free blocks (loop or stale entry)", node);
            }

            previous = node;
            node = node -> next;
        }
    }

    if(free_list_count != free_blocks_in_heap){
        return report("a free block is missing from the free lists", NULL);
    }

    return 1;
}


/* PUBLIC FUNCTIONS (these are the only ones that take locks) */

void* my_malloc(size_t size){

    //make sure input is valid
    if(size < 1){
        return NULL;
    }

    //absurdly big requests would overflow the math below
    if(size > SIZE_MAX / 2){
        return NULL;
    }

    //round up so every payload stays aligned
    size = payload_size_for(size);

    //huge requests skip the arenas completely
    if(size >= LARGE_THRESHOLD){
        return large_alloc(size);
    }

    //use this thread's own arena, so we only compete with threads that share it
    arena_t* a = get_thread_arena();

    LOCK_ARENA(a);
    void* result = malloc_unlocked(a, size);
    LINGER();
    UNLOCK_ARENA(a);

    return result;
}

//turns a pointer the user gave us back into its block header, or returns NULL (after
//printing why) if it cannot possibly be ours. these are cheap checks, not a guarantee
static header* block_from_ptr(void* ptr, const char* who){

    //real pointers are always aligned
    if((uintptr_t)ptr % ALIGNMENT != 0){
        log_error(who, "pointer was not allocated by my_malloc", ptr);
        return NULL;
    }

    //the header sits right before the payload we handed out
    header* block = (header*)((char*)ptr - sizeof(header));

    //the header tells us which arena owns this block
    int id = block -> arena_id;

    if(id == LARGE_ARENA_ID || id == LARGE_ALIGNED_ID){
        return block;
    }

    if(id < 0 || id >= MAX_ARENAS || arena_ready(&arenas[id]) == 0){
        log_error(who, "pointer was not allocated by my_malloc", ptr);
        return NULL;
    }

    return block;
}

void my_free(void* ptr){

    //freeing NULL is allowed and does nothing, just like the real free
    if(ptr == NULL){
        return;
    }

    header* block = block_from_ptr(ptr, "my_free: ");
    if(block == NULL){
        return;
    }

    //NOTE: this is the OWNER arena, which may not be the calling thread's arena
    //(that is a cross-thread free)
    int id = block -> arena_id;

    if(id == LARGE_ARENA_ID || id == LARGE_ALIGNED_ID){
        large_free(block);
        return;
    }

    //lock the OWNER's arena, not our own, so two threads never edit the same bins at once
    arena_t* a = &arenas[id];

    LOCK_ARENA(a);
    free_unlocked(a, block);
    LINGER();
    UNLOCK_ARENA(a);
}

void* my_realloc(void* ptr, size_t size){

    //realloc(NULL, n) is just malloc(n)
    if(ptr == NULL){
        return my_malloc(size);
    }

    //realloc(p, 0) frees the block
    if(size < 1){
        my_free(ptr);
        return NULL;
    }

    if(size > SIZE_MAX / 2){
        return NULL;
    }

    header* block = block_from_ptr(ptr, "my_realloc: ");
    if(block == NULL){
        return NULL;
    }

    size_t need = payload_size_for(size);
    int id = block -> arena_id;
    size_t old_size;

    if(id == LARGE_ARENA_ID){
        //staying big: let the kernel resize the mapping, no copying
        if(need >= LARGE_THRESHOLD){
            return large_realloc(block, need);
        }
        old_size = block -> size;
    }
    else if(id == LARGE_ALIGNED_ID){
        old_size = block -> size;
    }
    else{
        arena_t* a = &arenas[id];

        LOCK_ARENA(a);

        if(block -> is_free == 1){
            UNLOCK_ARENA(a);
            log_error("my_realloc: ", "block is already free", ptr);
            return NULL;
        }

        //first choice: resize where it sits
        if(realloc_in_place(a, block, need)){
            UNLOCK_ARENA(a);
            return ptr;
        }

        //read the size while we still hold the lock
        old_size = block -> size;
        UNLOCK_ARENA(a);
    }

    //second choice: new block, copy, free the old one. (no lock is held here, my_malloc and
    //my_free take their own, so we never hold two arena locks at once)
    void* fresh = my_malloc(size);
    if(fresh == NULL){
        return NULL;
    }

    memcpy(fresh, ptr, old_size < size ? old_size : size);
    my_free(ptr);
    return fresh;
}

void* my_calloc(size_t count, size_t size){

    //count * size can wrap around to a small number, which would hand out too little memory
    size_t total;
    if(__builtin_mul_overflow(count, size, &total)){
        return NULL;
    }

    void* p = my_malloc(total);
    if(p == NULL){
        return NULL;
    }

    //a block that was just mmap'd is already zero, so only arena blocks need clearing
    header* block = (header*)((char*)p - sizeof(header));
    if(block -> arena_id != LARGE_ARENA_ID){
        memset(p, 0, total);
    }

    return p;
}

void* my_memalign(size_t alignment, size_t size){

    //alignment must be a power of two
    if(alignment == 0 || (alignment & (alignment - 1)) != 0){
        return NULL;
    }
    if(size < 1 || size > SIZE_MAX / 2){
        return NULL;
    }

    //our normal blocks are already aligned this well
    if(alignment <= ALIGNMENT){
        return my_malloc(size);
    }

    return large_aligned_alloc(alignment, size);
}

size_t my_usable_size(void* ptr){
    if(ptr == NULL){
        return 0;
    }

    header* block = block_from_ptr(ptr, "my_usable_size: ");
    if(block == NULL || block -> is_free == 1){
        return 0;
    }

    return block -> size;
}

//throw away everything we know and start over. Meant for tests, call it when no other
//thread is using the allocator. chunks go back to the OS; any large blocks still
//in use are simply forgotten about
void my_heap_init(void){
    for(int i = 0; i < MAX_ARENAS; i++){
        arena_t* a = &arenas[i];
        if(arena_ready(a) == 0){
            continue;
        }

        LOCK_ARENA(a);

        //give every chunk back to the OS
        header* block = a -> first_block;
        while(block != NULL){
            footer* prologue = (footer*)((char*)block - sizeof(footer));

            //find the end of this chunk (the epilogue) to learn where the next chunk is
            header* walk = block;
            while(walk -> size != 0){
                walk = next_block(walk);
            }
            header* next_chunk = walk -> next;

            munmap((char*)prologue - CHUNK_LEAD, prologue -> chunk_len);
            block = next_chunk;
        }

        for(int b = 0; b < NUM_BINS; b++){
            a -> bins[b] = NULL;
        }
        a -> bin_bitmap = 0;
        a -> first_block = NULL;
        a -> last_epilogue = NULL;
        a -> mapped_bytes = 0;

        UNLOCK_ARENA(a);
    }

    __atomic_store_n(&large_mapped_bytes, 0, __ATOMIC_RELAXED);
}

//checks every arena, one at a time (never holding two locks)
int my_heap_check(void){
    for(int i = 0; i < MAX_ARENAS; i++){
        arena_t* a = &arenas[i];
        if(arena_ready(a) == 0){
            continue;
        }

        LOCK_ARENA(a);
        int ok = check_arena(a);
        UNLOCK_ARENA(a);

        if(!ok){
            return 0;
        }
    }
    return 1;
}

//fills in totals across all arenas. numbers are a snapshot, other threads may be moving
void my_heap_stats(my_heap_stats_t* stats){
    stats -> mapped_bytes = 0;
    stats -> free_bytes = 0;
    stats -> free_blocks = 0;
    stats -> largest_free_block = 0;
    stats -> arenas_with_memory = 0;
    stats -> used_bytes = 0;
    stats -> used_blocks = 0;

    for(int i = 0; i < MAX_ARENAS; i++){
        arena_t* a = &arenas[i];
        if(arena_ready(a) == 0){
            continue;
        }

        LOCK_ARENA(a);

        stats -> mapped_bytes += a -> mapped_bytes;
        if(a -> mapped_bytes > 0){
            stats -> arenas_with_memory++;
        }

        //walk every block of every chunk to total up the ones that are in use
        header* walk = a -> first_block;
        while(walk != NULL){
            while(walk -> size != 0){
                if(walk -> is_free == 0){
                    stats -> used_bytes += walk -> size + OVERHEAD;
                    stats -> used_blocks++;
                }
                walk = next_block(walk);
            }
            //we are on the epilogue now, its next pointer leads to the next chunk
            walk = walk -> next;
        }

        for(int b = 0; b < NUM_BINS; b++){
            for(header* node = a -> bins[b]; node != NULL; node = node -> next){
                stats -> free_bytes += node -> size;
                stats -> free_blocks++;
                if(node -> size > stats -> largest_free_block){
                    stats -> largest_free_block = node -> size;
                }
            }
        }

        UNLOCK_ARENA(a);
    }

    //large blocks count as mapped memory too, and they are always in use
    size_t large_bytes = __atomic_load_n(&large_mapped_bytes, __ATOMIC_RELAXED);
    stats -> mapped_bytes += large_bytes;
    stats -> used_bytes += large_bytes;
}


/* FORK SAFETY */

//fork() copies only the thread that called it. if another thread was holding an arena lock at that
//moment, the child would own a lock nobody will ever release and hang on its first malloc.
//the fix: take every lock just before the fork so none is held by anyone else, then release
//them in both the parent and the child. (locks are always taken in the same order: table, then arenas by id)
static void fork_prepare(void){
    pthread_mutex_lock(&arena_table_lock);
    for(int i = 0; i < MAX_ARENAS; i++){
        if(arena_ready(&arenas[i])){
            LOCK_ARENA(&arenas[i]);
        }
    }
}

static void fork_release(void){
    for(int i = MAX_ARENAS - 1; i >= 0; i--){
        if(arena_ready(&arenas[i])){
            UNLOCK_ARENA(&arenas[i]);
        }
    }
    pthread_mutex_unlock(&arena_table_lock);
}

//runs once when the program (or the preloaded library) starts, before main
__attribute__((constructor))
static void register_fork_handlers(void){
    pthread_atfork(fork_prepare, fork_release, fork_release);
}
