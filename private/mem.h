#ifndef MEM_H_
#define MEM_H_
#include <stddef.h>

struct slab {
    size_t obj_sz;
    size_t page_sz;
    void* cur;
    void* page_list;
};

void slab_create( struct slab* , size_t sz , size_t page_sz );
void slab_destroy( struct slab* );
void* slab_malloc( struct slab* );
void slab_free( struct slab* , void* ptr );

#endif /* MEM_H_ */

