#include <stdio.h>
#include "include/malloc.h"

int main()
{
    // start with a clean heap
    my_heap_init();

    // ask for space
    int *p = my_malloc(sizeof(int));

    // check if it returned null
    if(p == NULL){
        //print error statement
        printf("Error, memory allocation failed.\n");
        //end
        return -1;
    }
    // store
    *p = 10;

    // calculate
    *p = *p + 10;

    //print
    printf("Should be 20, : %d\n", *p);

    // make sure the heap is still healthy
    printf("Heap check: %s\n", my_heap_check() ? "ok" : "CORRUPTED");

    // release space of pointer p (our free, not the real one)
    my_free(p);

    // prevent dangling pointer
    p = NULL;

    return 0;
}
