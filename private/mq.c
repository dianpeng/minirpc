#include "mq.h"
#include "conf.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>


/* This message queue is not a lock-free data structure since I am not smart enough to write one :)
 * It is a still non wait-free queue. However, with some modification to make the contention has
 * really small time window. The core is that we have two internal queues instead of one, one for dequeue,
 * one for enqueue. The user just call mq_dequeue, until the current dequeued message queue runs out of
 * its resources. Then it will try to SWAP the queue, this SWAP operation can introduce contention and
 * it will ONLY happened in the dequeue side, so it means, the enqueue side will NEVER try to swap
 * the queue. But it needs to grab a lock to let the dequeue side knows that I am working . The key here
 * is that I use a spin lock. Why spin lock works here ? It is because, in most cases, such SWAP should be
 * very quick. Since th mq_enqueue side is just insert pointer, it may have memory allocation , but not
 * inside of the lock scope. And also swap the pointer is _VERY_ fast. So a spin lock works here.
 * On Windows, no spin lock is here, but Critical Section initialized with spin count may lead better performance
 * when unexpected long lock hold is there, so we just use critical section ; on Posix compatible platform,
 * pthread_spinlock is the candidate there. No pthread_mutex since futex is definitely working at kernel level. */

#ifdef _WIN32
#include <Windows.h>
typedef CRITICAL_SECTION spinlock_t;
static
void spinlock_init( spinlock_t* l ) {
    InitializeCriticalSectionAndSpinCount(l,2000);
}
static
void spinlock_lock( spinlock_t* l ) {
    EnterCriticalSection(l);
}
static
void spinlock_unlock( spinlock_t* l ) {
    LeaveCriticalSection(l);
}
static
void spinlock_delete( spinlock_t* l ) {
    DeleteCriticalSection(l);
}
#else
#include <pthread.h>
typedef pthread_spinlock_t spinlock_t;
static
void spinlock_init( spinlock_t* lk ) {
#ifndef NDEBUG
    int ret =
#endif /* NDEBUG */
        pthread_spin_init(lk,PTHREAD_PROCESS_PRIVATE);
    assert( ret == 0 );
}
static
void spinlock_delete( spinlock_t* lk ) {
#ifndef NDEBUG
    int ret =
#endif /* NDEBUG */
        pthread_spin_destroy(lk);
    assert( ret == 0 );
}
static
void spinlock_lock( spinlock_t* lk ) {
#ifndef NDEBUG
    int ret =
#endif /* NDEBUG */
        pthread_spin_lock(lk);
    assert( ret == 0 );
}
static
void spinlock_unlock( spinlock_t* lk ) {
#ifndef NDEBUG
    int ret =
#endif /* NDEBUG */
        pthread_spin_unlock(lk);
    assert( ret == 0 );
}

#endif /* _WIN32 */

/* A real queue implementation, all the operation related
 * to this queue is not thread safe or atomic */

struct queue_node_t {
    void* data;
    struct queue_node_t* next;
    struct queue_node_t* prev;
};

struct queue_t {
    struct queue_node_t tail;
    size_t size;
};

static
void initqueue ( struct queue_t* q ) {
    q->tail.next = &(q->tail);
    q->tail.prev = &(q->tail);
    q->tail.data = NULL;
    q->size = 0;
}

static
void enqueue( struct queue_t* q , struct queue_node_t* n ) {
    q->tail.prev->next = n;
    n->prev = q->tail.prev;
    n->next = &(q->tail);
    q->tail.prev = n;
    ++(q->size);
}

static
int dequeue( struct queue_t* q , struct queue_node_t** n ) {
   if( q->size == 0 )
       return -1;
   *n = q->tail.next;
   q->tail.next = (*n)->next;
   (*n)->next->prev = &(q->tail);
   --(q->size);
   return 0;
}

static
void clearqueue( struct queue_t* q ) {
    struct queue_node_t* n = q->tail.next;
    struct queue_node_t* tp;
    while( n != &(q->tail) ) {
        tp = n->next;
        free(n);
        n = tp;
    }
}

struct mq_t {
    struct queue_t in_queue , out_queue;
    struct queue_t* fqptr , *bqptr;
    spinlock_t fr_lk , bk_lk;
};

/* enqueue operations will only affect the front queue pointer */
void mq_enqueue( struct mq_t* mq , void* data ) {
    /* do the allocation */
    struct queue_node_t* n = malloc(sizeof(*n));
    n->data = data;
    VERIFY(n);
    /* lock the queue since we may have contention */
    spinlock_lock(&(mq->fr_lk));
    /* insert the data into the front queue now */
    enqueue(mq->fqptr,n);
    /* unlock the spinlock */
    spinlock_unlock(&(mq->fr_lk));
}

int mq_dequeue( struct mq_t* mq, void** data ) {
    /* get data from the back queue no lock now. */
    struct queue_node_t* n;
    int ret;
    
    /* try to dequeue the data from the queue */
    spinlock_lock(&(mq->bk_lk));
    ret = dequeue(mq->bqptr,&n);
    spinlock_unlock(&(mq->bk_lk));


    if( ret != 0 ) {
        struct queue_t* ptr;

        /* not working , we need to swap the queue now */
        spinlock_lock(&(mq->bk_lk));
        spinlock_lock(&(mq->fr_lk));
        ptr = mq->bqptr;
        mq->bqptr = mq->fqptr;
        mq->fqptr = ptr;
        spinlock_unlock(&(mq->fr_lk));
        spinlock_unlock(&(mq->bk_lk));

        /* try to dequeue again */
        spinlock_lock(&(mq->bk_lk));
        ret = dequeue(mq->bqptr,&n);
        spinlock_unlock(&(mq->bk_lk));
        if( ret != 0 ) 
            return -1;
    }
    *data = n->data;
    free(n);
    return 0;
}

struct mq_t* mq_create() {
    struct mq_t* ret = malloc( sizeof(*ret) );
    VERIFY(ret);
    initqueue(&(ret->in_queue));
    initqueue(&(ret->out_queue));
    ret->bqptr = &(ret->in_queue);
    ret->fqptr = &(ret->out_queue);
    spinlock_init(&(ret->fr_lk));
    spinlock_init(&(ret->bk_lk));
    return ret;
}

void mq_destroy( struct mq_t* mq ) {
    clearqueue(&(mq->in_queue));
    clearqueue(&(mq->out_queue));
    spinlock_delete(&(mq->bk_lk));
    spinlock_delete(&(mq->fr_lk));
}
