#include "minirpc-service.h"
#include "private/conf.h"
#include "minirpc.h"

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <process.h>
typedef HANDLE th_hander;
#else
#include <pthread.h>
#include <unistd.h>
typedef pthread_t th_hander;
#endif

struct mrpc_service_entry;
struct mrpc_service_table;

struct mrpc_service_entry {
    char method_name[ MRPC_MAX_METHOD_NAME_LEN ];
    void* udata;
    mrpc_service_cb func;
    struct mrpc_service_entry* next;
    int fhash;
};

struct mrpc_service_table {
    struct mrpc_service_entry* array;
    size_t cap;
    size_t size;
};

static
int _mrpc_stbl_calc_hash( const char* key , size_t len ) {
    int val = 0;
    size_t i ;
    for( i = 0 ; i < len ; ++i ) {
        val = val ^ ((val<<5)+(val>>2)+(int)key[i]);
    }
    return val;
}

static
void mrpc_stbl_init( struct mrpc_service_table* tbl , size_t cap ) {
    tbl->array = malloc( sizeof(struct mrpc_service_entry) * cap );
    VERIFY(tbl->array);
    tbl->cap = cap;
    tbl->size= 0;
    memset(tbl->array,0,sizeof(struct mrpc_service_entry)*cap);
}

static
int _mrpc_stbl_query_slot( const struct mrpc_service_table* tbl , const struct mrpc_service_entry* entry ) {
    int idx = entry->fhash % tbl->cap;
    struct mrpc_service_entry* tar;

    if( tbl->array[idx].func == NULL ) {
        tar = tbl->array + idx;
    } else {
        struct mrpc_service_entry* ent;
        int h = entry->fhash;

        /* find out where we should insert such entry */
        ent = (tbl->array+idx);
        while( ent->next != NULL ) {
            if( strcmp(ent->method_name,entry->method_name) == 0 )
                return -1;
            ent = ent->next;
        }
        if( strcmp(ent->method_name,entry->method_name) == 0 )
            return -1;

        /* probing to where we should insert this entry */
        while( tbl->array[ ++h % tbl->cap ].func != NULL ) {}
        tar = tbl->array + (h % tbl->cap);
        ent->next = tar;
    }

    /* do the insertion here */
    memcpy(tar,entry,sizeof(*entry));
    tar->next = NULL;
    return tar-(tbl->array);
}

static
void
_mrpc_stbl_rehash( struct mrpc_service_table* tbl ) {
    struct mrpc_service_table tmp_tb;
    int idx;
    size_t i;

    assert(tbl->cap == tbl->size);

    mrpc_stbl_init(&tmp_tb,tbl->cap*2);

    for( i = 0 ; i < tbl->size ; ++i ) {
        idx = _mrpc_stbl_query_slot(tbl,tbl->array+i);
        assert( idx >= 0 );
    }
    tbl->cap = tmp_tb.cap;
    tbl->array = tmp_tb.array;
    free(tbl->array);
}

static
int mrpc_stbl_insert( struct mrpc_service_table* tbl ,
    mrpc_service_cb cb , const char* method_name , void* udata ) {
    struct mrpc_service_entry tmp_ent;
    size_t len = strlen(method_name);
    int idx;

    if( len >= MRPC_MAX_METHOD_NAME_LEN )
        return -1;
    strcpy(tmp_ent.method_name,method_name);
    tmp_ent.fhash = _mrpc_stbl_calc_hash(method_name,len);
    tmp_ent.func = cb;
    tmp_ent.next = NULL;
    tmp_ent.udata = udata;

    /* checking if we need to rehash the hash table here */
    if( tbl->size == tbl->cap )
        _mrpc_stbl_rehash(tbl);
    assert(tbl->size < tbl->cap);

    idx = _mrpc_stbl_query_slot(tbl,&tmp_ent);
    if( idx < 0 )
        return -1;

    /* since mrpc_stbl_query_slot accept a const table
       pointer , so we need to bump its size */
    ++(tbl->size);

    return 0;
}

static
const struct mrpc_service_entry*
mrpc_stbl_query( struct mrpc_service_table* tbl , const char* method_name ) {
    int len = strlen(method_name);
    int fhash ;
    int idx ;
    struct mrpc_service_entry* ent;

    if( len >= MRPC_MAX_METHOD_NAME_LEN )
        return NULL;

    fhash = _mrpc_stbl_calc_hash(method_name,len);
    idx = fhash % tbl->cap;
    ent = tbl->array + idx;

    if( ent->func == NULL )
        return NULL;
    else {
        while( ent->fhash != fhash || strcmp(ent->method_name,method_name) != 0 ) {
            ent = ent->next;
            if( ent == NULL )
                return NULL;
        }
        return ent;
    }
}

static
void
mrpc_stbl_destroy( struct mrpc_service_table* tbl ) {
    free(tbl->array);
    tbl->cap = tbl->size = 0;
}

/* thread utility */
struct th_pool {
    th_hander* handles;
    size_t th_sz;
};

struct mrpc_service_th {
    int exit; /* This one is used to release the thread when error happened */
    struct mrpc_service* service;
};

typedef void (*th_cb)(void*);
struct th_data {
    struct mrpc_service_th p;
    th_cb cb;
};


#ifdef _WIN32
static
unsigned int
_stdcall
_th_pool_entry( void* p ) {
    struct th_data* d = CAST(struct th_data*,p);
    d->cb(&(d->p));
    return 0;
}
#else
static
void*
_th_pool_entry( void* p ) {
    struct th_data* d = CAST(struct th_data*,p);
    d->cb(&(d->p));
    return NULL;
}
#endif /* _WIN32 */

static
int
th_pool_create( struct th_pool* pool , size_t th_sz , th_cb cb ,
                struct th_data* data , int* created_sz ) {
    size_t i;

    *created_sz = 0;
    pool->handles = malloc(th_sz*sizeof(th_hander));
    VERIFY(pool->handles);
    pool->th_sz = th_sz;

    for( i = 0 ; i < th_sz ; ++i ) {
#ifdef _WIN32
        pool->handles[i] =  (th_hander)_beginthreadex(
        NULL,
        0,
        _th_pool_entry,
        data+i,
        0,
        NULL);
        if( pool->handles[i] == INVALID_HANDLE_VALUE ) {
            return -1;
        }
        ++*created_sz;
#else
        int pthread_ret =
            pthread_create(&(pool->handles[i]),NULL,_th_pool_entry,data+i);
        if( pthread_ret != 0 ) {
            return -1;
        }
        ++*created_sz;
#endif /* _WIN32 */
    }

    return 0;
}

static
int
th_pool_join( struct th_pool* pool , int cnt ) {
#ifdef _WIN32
    int wait_batch = 0;
    int left_wait = cnt;
    int offset = 0;

    DWORD dwRet;
    assert(pool->th_sz !=0);
    assert(pool->th_sz > (size_t)cnt);
    do {
        wait_batch = MIN(MAXIMUM_WAIT_OBJECTS,left_wait);
        dwRet = WaitForMultipleObjects(
            wait_batch,
            pool->handles+offset,
            TRUE,
            INFINITE);

        if( dwRet == WAIT_OBJECT_0 ) {
            left_wait -= wait_batch;
            offset += wait_batch;
            if( left_wait == 0 )
                return 0;
        } else {
            return -1;
        }

    } while(1);

#else
    /* Pthread doesn't comes with pthread_multi_join , so we just join each thread */
    assert( cnt < pool->th_sz );
    for( ; cnt != 0 ; --cnt ) {
        int ret = pthread_join( pool->handles[cnt] , NULL );
        if( ret != 0 )
            return -1;
    }
    return 0;
#endif /* _WIN32 */
}

static
void
th_pool_destroy( struct th_pool* pool ) {
    free(pool->handles);
    pool->handles = NULL;
    pool->th_sz = 0;
}

/* MRPC service implementation */
struct mrpc_service {
    void* udata;
    struct mrpc_service_table stable;
    struct th_data* th_data; /* the size of this data is same as thread pool th_sz */
    struct th_pool th_pool;
    size_t min_slp_tm; /* min sleep timeout */
    size_t max_slp_tm; /* max sleep timeout */
};

/* Thread creation is typically a memory barrier , so after creating thread
 * all the data that is visible to this thread, but not including the data
 * modification happens right after the thread creation */

static
void
_mrpc_service_th_cb( void* par ) {
    struct mrpc_service_th* th = CAST( struct mrpc_service_th* , par );
    while(!th->exit) {
        void* key;
        struct mrpc_request req;
        const struct mrpc_service_entry* func_entry;
        int ret = mrpc_request_recv(&req,&key);

        if( ret <0 )
            continue;

        /* when we get the notification that MRPC is interrupted, we just
         * return from the thread callback and user needs to call mrpc_service_quit
         * to join all the allocated thread */

        else if( ret == 1 )
            return;

        /* look up the service and then start to execute */
        func_entry = mrpc_stbl_query( &(th->service->stable), req.method_name );
        if( func_entry == NULL ) {

            mrpc_response_send(
                CAST(const struct mrpc_request*,&req),
                key,
                NULL,
                MRPC_EC_FUNCTION_NOT_FOUND);

        } else {
            int error_code;
            struct mrpc_val result;

            func_entry->func(
                th->service,
                &req,
                func_entry->udata,
                &error_code,
                &result);
            /* sending the response to the MRPC out band queue */
            mrpc_response_send(
                &req,
                key,
                &result,
                error_code);
        }
    }
}

struct mrpc_service*
mrpc_service_create( size_t sz , size_t min_slp_time , size_t max_slp_time , void* opaque ) {
    struct mrpc_service* ret = malloc(sizeof(*ret));

    VERIFY(ret);
    VERIFY(sz !=0);

    mrpc_stbl_init(&ret->stable,sz);

    ret->th_data = NULL;
    ret->th_pool.handles = NULL;
    ret->th_pool.th_sz = 0;

    ret->udata = opaque;
    ret->max_slp_tm = max_slp_time;
    ret->min_slp_tm = min_slp_time;

    return ret;
}

void mrpc_service_destroy( struct mrpc_service* service ) {
    if( service->th_data != NULL ) {
        free(service->th_data);
    }
    if( service->th_pool.th_sz != 0 ) {
        th_pool_destroy(&(service->th_pool));
    }
    mrpc_stbl_destroy(&(service->stable));
    free(service);
}

int mrpc_service_add( struct mrpc_service* service , mrpc_service_cb cb , const char* method_name , void* udata ) {
    return mrpc_stbl_insert( &(service->stable), cb , method_name , udata );
}

void mrpc_service_run_once( struct mrpc_service* service ) {
    void* key;
    struct mrpc_request req;
    const struct mrpc_service_entry* func_entry;
    if( mrpc_request_try_recv(&req,&key) == 0 ) {
        /* look up the service and then start to execute */
        func_entry = mrpc_stbl_query( &(service->stable), req.method_name );
        if( func_entry == NULL ) {

            mrpc_response_send(
                &req,
                key,
                NULL,
                MRPC_EC_FUNCTION_NOT_FOUND);

        } else {
            int error_code;
            struct mrpc_val result;

            func_entry->func(
                service,
                &req,
                func_entry->udata,
                &error_code,
                &result);
            /* sending the response to the MRPC out band queue */
            mrpc_response_send(
                &req,
                key,
                &result,
                error_code);
        }
    }
}

void mrpc_service_run( struct mrpc_service* service ) {
    struct mrpc_service_th th;
    th.service = service;
    _mrpc_service_th_cb(&th);
}

int mrpc_service_run_remote( struct mrpc_service* service, int thread_sz ) {
    int i;
    int created_sz;
    int ret;

    service->th_data = malloc(sizeof(*service->th_data)*thread_sz);
    VERIFY(service->th_data);
    for( i = 0 ; i < thread_sz ; ++i ) {
        service->th_data[i].p.service = service;
        service->th_data[i].cb = _mrpc_service_th_cb;
        service->th_data[i].p.exit = 0;
    }

    ret=th_pool_create(&(service->th_pool),thread_sz,
                   _mrpc_service_th_cb,
                   service->th_data,
                   &created_sz);

    if( ret != 0 ) {
        /* rollback to no thread creation status */
        for( i = 0 ; i < created_sz ; ++i ) {
            service->th_data[i].p.exit = 1;
        }
        th_pool_join(&(service->th_pool),created_sz);
        return -1;
    }

    return 0;
}

int mrpc_service_quit( struct mrpc_service* service ) {
    return th_pool_join(&(service->th_pool),service->th_pool.th_sz);
}

void* mrpc_service_get_udata( struct mrpc_service* service ) {
    return service->udata;
}
