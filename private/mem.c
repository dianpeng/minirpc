#include "mem.h"
#include "conf.h"
#include <stdlib.h>

struct page {
    struct page* next;
};

struct header {
    struct header* next;
};

/* The size is in bytes */
static
void grow( struct slab* slab ) {
    size_t psz = slab->page_sz * 2;
    void* new_pool = malloc( psz * slab->obj_sz + sizeof(void*) );
    struct header* h ;
    struct header* ptr;
    size_t i = 0;

    VERIFY(new_pool);

    /* link into the slab page queue */
    CAST(struct page*,new_pool)->next = 
        CAST(struct page*,slab->page_list);
    slab->page_list = new_pool;
    new_pool=CAST(char*,new_pool)+sizeof(void*);

    ptr = CAST(struct header*,new_pool);
    h=ptr;
    for( ; i < psz-1 ; ++i ) {
        ptr->next = CAST(struct header*,CAST(char*,ptr) + slab->obj_sz);
        ptr = CAST(struct header*,CAST(char*,ptr)+slab->obj_sz);
    }
    ptr->next = CAST(struct header*,slab->cur);
    slab->cur = h;
    slab->page_sz = psz;
}

#define ALIGN(x,a) (((x) + (a) - 1) & ~((a) - 1))

void slab_create( struct slab* slb , size_t sz , size_t page_sz ) {
    assert(page_sz != 0);
    assert(sz != 0);
    slb->obj_sz = ALIGN(sz,sizeof(void*));
    slb->page_sz = page_sz;
    slb->cur = slb->page_list = NULL;
    grow(slb);
}

void slab_destroy( struct slab* slb ) {
    while( slb->page_list != NULL ) {
        void* n = CAST(struct page*,slb->page_list)->next;
        free(slb->page_list);
        slb->page_list = n;
    }
    slb->cur = slb->page_list = NULL;
    slb->obj_sz = slb->page_sz = 0;
}

void* slab_malloc( struct slab* slb ) {
    void* next;
    if( slb->cur == NULL )
        grow(slb);
    assert(slb->cur != NULL);
    next = slb->cur;
    slb->cur = CAST(struct header*,next)->next;
    return next;
}

void slab_free( struct slab* slb , void* ptr ) {
    CAST(struct header*,ptr)->next = CAST(struct header*,slb->cur);
    slb->cur = ptr;
}
