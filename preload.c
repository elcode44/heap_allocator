/*
 * preload.c : turns this allocator into a drop-in replacement for the C library's malloc
 * ------------------------------------------------------------------------------------
 * Build it into a shared library and run any program on it:
 *
 *      make lib
 *      LD_PRELOAD=./libmyalloc.so ls -R /usr
 *
 * LD_PRELOAD tells the dynamic loader "look in this library FIRST when resolving symbols".
 * Because this file defines malloc, free, calloc, realloc... the program (and every library
 * it uses, including libc itself) ends up calling OUR versions instead of glibc's.
 *
 * Each function is a thin wrapper that adapts the C library's rules to the my_* functions:
 *   - malloc(0) must return a real, unique pointer (many programs treat NULL as "out of memory"),
 *     so size 0 is turned into size 1
 *   - failures must set errno to ENOMEM
 *   - posix_memalign / aligned_alloc / memalign / valloc must all hand out memory that free() accepts,
 *     otherwise the program would pass glibc's memory to OUR free (or the other way round)
 *
 * Set MYALLOC_STATS=1 to print a one-line summary to stderr when the program exits, or
 * MYALLOC_STATS=/some/file to append it to a file instead (many tools, like ls and sort, close stderr
 * before exiting). It is also a quick way to prove the library really was in use.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>
#include "include/malloc.h"

void* malloc(size_t size){
    void* p = my_malloc(size == 0 ? 1 : size);
    if(p == NULL){
        errno = ENOMEM;
    }
    return p;
}

void free(void* ptr){
    my_free(ptr);
}

void* calloc(size_t count, size_t size){
    if(count == 0 || size == 0){
        count = 1;
        size = 1;
    }
    void* p = my_calloc(count, size);
    if(p == NULL){
        errno = ENOMEM;
    }
    return p;
}

void* realloc(void* ptr, size_t size){

    //realloc(NULL, 0) must behave like malloc(0): a real pointer (grep, for one, depends on this)
    if(ptr == NULL){
        return malloc(size);
    }

    void* p = my_realloc(ptr, size);

    //realloc(p, 0) frees p and returns NULL on purpose, that is not an error
    if(p == NULL && size != 0){
        errno = ENOMEM;
    }
    return p;
}

void* memalign(size_t alignment, size_t size){
    void* p = my_memalign(alignment, size == 0 ? 1 : size);
    if(p == NULL){
        errno = ENOMEM;
    }
    return p;
}

int posix_memalign(void** out, size_t alignment, size_t size){
    //the rules: a power of two, and a multiple of sizeof(void*)
    if(alignment % sizeof(void*) != 0 || alignment == 0 || (alignment & (alignment - 1)) != 0){
        return EINVAL;
    }

    void* p = my_memalign(alignment, size == 0 ? 1 : size);
    if(p == NULL){
        return ENOMEM;
    }

    *out = p;
    return 0;
}

void* aligned_alloc(size_t alignment, size_t size){
    return memalign(alignment, size);
}

void* valloc(size_t size){
    return memalign((size_t)sysconf(_SC_PAGESIZE), size);
}

void* pvalloc(size_t size){
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    //round the size up to whole pages
    size_t rounded = (size + page - 1) & ~(page - 1);
    if(rounded < size){
        errno = ENOMEM;
        return NULL;
    }
    return memalign(page, rounded == 0 ? page : rounded);
}

size_t malloc_usable_size(void* ptr){
    return my_usable_size(ptr);
}


/* MYALLOC_STATS=1 : print a summary when the program exits */

//write a number in decimal without stdio (printf could call malloc)
static void put_number(char* buf, size_t* len, size_t cap, unsigned long value){
    char digits[24];
    int count = 0;
    do{
        digits[count++] = (char)('0' + value % 10);
        value /= 10;
    } while(value != 0);
    while(count > 0 && *len < cap){
        buf[(*len)++] = digits[--count];
    }
}

static void put_text(char* buf, size_t* len, size_t cap, const char* text){
    while(*text != '\0' && *len < cap){
        buf[(*len)++] = *text++;
    }
}

__attribute__((destructor))
static void print_stats_at_exit(void){
    const char* where = getenv("MYALLOC_STATS");
    if(where == NULL){
        return;
    }

    my_heap_stats_t s;
    my_heap_stats(&s);

    char buf[256];
    size_t len = 0;
    put_text(buf, &len, sizeof(buf), "[myalloc] arenas used: ");
    put_number(buf, &len, sizeof(buf), (unsigned long)s.arenas_with_memory);
    put_text(buf, &len, sizeof(buf), ", mapped from OS: ");
    put_number(buf, &len, sizeof(buf), (unsigned long)(s.mapped_bytes / 1024));
    put_text(buf, &len, sizeof(buf), " KB, blocks still in use: ");
    put_number(buf, &len, sizeof(buf), (unsigned long)s.used_blocks);
    put_text(buf, &len, sizeof(buf), ", free blocks: ");
    put_number(buf, &len, sizeof(buf), (unsigned long)s.free_blocks);
    put_text(buf, &len, sizeof(buf), "\n");

    //a value that looks like a path means "append to that file"
    int fd = 2;
    if(where[0] == '/'){
        fd = open(where, O_WRONLY | O_APPEND | O_CREAT, 0644);
        if(fd < 0){
            return;
        }
    }

    ssize_t ignored = write(fd, buf, len);
    (void)ignored;

    if(fd != 2){
        close(fd);
    }
}
