#ifndef MEM_H_
#define MEM_H_
#include <stddef.h>

struct slab_t {
    size_t obj_sz;
    size_t page_sz;
    void* cur;
    void* page_list;
};

void slab_create( struct slab_t* , size_t sz , size_t page_sz );
void slab_destroy( struct slab_t* );
void* slab_malloc( struct slab_t* );
void slab_free( struct slab_t* , void* ptr );

#endif /* MEM_H_ */

