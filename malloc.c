#include <unistd.h>    /* sysconf */
#include <stdio.h>     /* fprintf, only used to print error messages */
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
 * Known limitations (also good things to say out loud in an interview):
 *  - chunks are never returned to the OS, they are only reused
 *  - if a thread's arena is out of memory we fail, we don't borrow from others
 *  - threads are spread over MAX_ARENAS arenas round-robin; past that they share
 *  - an arena is not recycled when its thread exits
 *  - double-freeing a LARGE block is not detected (the memory is already unmapped)
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
static __thread arena_t* thread_arena = NULL;

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

    int id = next_arena_id % MAX_ARENAS;
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
        fprintf(stderr, "my_free: double free detected on %p\n", (void*)((char*)block + sizeof(header)));
        return;
    }

    //mark it free
    block -> is_free = 1;

    //merge with free neighbors
    block = coalesce(a, block);

    //put the (possibly bigger) block into the right bin
    add_to_free_list(a, block);
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
    //walk back to where the mapping really starts
    void* base = (char*)block - HEADER_OFFSET;
    size_t total = HEADER_OFFSET + sizeof(header) + block -> size;
    __atomic_fetch_sub(&large_mapped_bytes, total, __ATOMIC_RELAXED);

    //give the pages straight back to the OS
    munmap(base, total);
}


/* HEAP CHECKER (assumes the caller already holds the arena's lock) */

//print what went wrong and return 0 so the caller can do: return report(...)
static int report(const char* message, void* where){
    fprintf(stderr, "my_heap_check: %s (at %p)\n", message, where);
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
    UNLOCK_ARENA(a);

    return result;
}

void my_free(void* ptr){

    //freeing NULL is allowed and does nothing, just like the real free
    if(ptr == NULL){
        return;
    }

    //real pointers are always 16 byte aligned
    if((uintptr_t)ptr % ALIGNMENT != 0){
        fprintf(stderr, "my_free: pointer %p was not allocated by my_malloc\n", ptr);
        return;
    }

    //the header sits right before the payload we handed out
    header* block = (header*)((char*)ptr - sizeof(header));

    //the header tells us which arena owns this block. NOTE: this is the OWNER, which
    //may not be the calling thread's arena (that is a cross-thread free)
    int id = block -> arena_id;

    if(id == LARGE_ARENA_ID){
        large_free(block);
        return;
    }

    if(id < 0 || id >= MAX_ARENAS || arena_ready(&arenas[id]) == 0){
        fprintf(stderr, "my_free: pointer %p was not allocated by my_malloc\n", ptr);
        return;
    }

    //lock the OWNER's arena, not our own, so two threads never edit the same bins at once
    arena_t* a = &arenas[id];

    LOCK_ARENA(a);
    free_unlocked(a, block);
    UNLOCK_ARENA(a);
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
