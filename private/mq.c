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
#ifdef WINVER
#if WINVER < 0x0600
#error "The code must be compiled at least Windows Vista"
#endif /* WINVER */
#endif /* WINVER */

typedef CRITICAL_SECTION mutex_t;
static
void mutex_init( mutex_t* l ) {
    InitializeCriticalSectionAndSpinCount(l,2000);
}
static
void mutex_lock( mutex_t* l ) {
    EnterCriticalSection(l);
}
static
void mutex_unlock( mutex_t* l ) {
    LeaveCriticalSection(l);
}
static 
int mutex_try_lock( mutex_t* l ) {
    return TryEnterCriticalSection(l) == TRUE ? 0 : -1;
}

static
void mutex_delete( mutex_t* l ) {
    DeleteCriticalSection(l);
}

/*
 * Condition variable on Windows. We don't provide implementation
 * for supporting less than Windows XP version. Use native CV is 
 * fine here 
 */

typedef CONDITION_VARIABLE cond_t;
void cond_init( cond_t* c ) {
    InitializeConditionVariable(c);
}

void cond_wait( cond_t* c , mutex_t* m , int msec ) {
#ifndef NDEBUG
    BOOL bret = 
#endif /* NDEBUG */
        SleepConditionVariableCS(c,m, msec < 0 ? INFINITE : (DWORD)(msec));
    assert(bret);
}

void cond_signal_one( cond_t* c  ) {
    WakeConditionVariable(c);
}

void cond_signal_all( cond_t* c ) {
    WakeAllConditionVariable(c);
}

void cond_delete( cond_t* c ) {
    c=c;
}

#else
#include <pthread.h>
typedef pthread_mutex_t mutex_t;

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
    struct queue_t q;
    mutex_t lk;
    cond_t c;
};

/* enqueue operations will only affect the front queue pointer */
void mq_enqueue( struct mq_t* mq , void* data ) {
    /* do the allocation */
    struct queue_node_t* n = malloc(sizeof(*n));
    n->data = data;
    VERIFY(n);
    /* lock the queue since we may have contention */
    mutex_lock(&(mq->lk));
    /* insert the data into the front queue now */
    enqueue(&(mq->q),n);
    /* unlock the spinlock */
    mutex_unlock(&(mq->lk));
    /* notify the mq_dequeue */
    cond_signal_one(&(mq->c));
}

#define MAX_SPIN 10

void mq_dequeue( struct mq_t* mq, void** data ) {
    /* get data from the back queue no lock now. */
    struct queue_node_t* n;
    int ret;

    /* try to dequeue the data from the queue */
    mutex_lock(&(mq->lk));
    ret = dequeue(&(mq->q),&n);
    mutex_unlock(&(mq->lk));

    if( ret != 0 ) {
        /* here means that we have already find out that the queue is empty.
      we can simply do a spin here to avoid fast sleep here */
        int i = 0;
        while( 1 ) {
            /* Try to do the dequeue */
            if( mutex_try_lock(&(mq->lk)) == 0 ) {
                if( dequeue(&(mq->q),&n) ==0 ) {
                    mutex_unlock(&(mq->lk));
                    goto done;
                } else {
                    mutex_unlock(&(mq->lk));
                }
            }
            if( ++i == MAX_SPIN ) {
                /* sleep */
                mutex_lock(&(mq->lk));
                while( dequeue(&mq->q,&n) != 0 ) {
                    cond_wait(&(mq->c),&(mq->lk),-1);
                }
                mutex_unlock(&(mq->lk));
                goto done;
            }
        }
    } 

done:
    *data = n->data;
    free(n);
}

int mq_try_dequeue( struct mq_t* mq , void** data ) {
    struct queue_node_t* n;
    mutex_lock(&(mq->lk));
    if( dequeue(&(mq->q),&n) == 0 ) {
        *data = n->data;
        free(n);
        mutex_unlock(&(mq->lk));
        return 0;
    } else {
        mutex_unlock(&(mq->lk));
        return -1;
    }
}

struct mq_t* mq_create() {
    struct mq_t* ret = malloc( sizeof(*ret) );
    VERIFY(ret);
    initqueue(&(ret->q));
    cond_init(&(ret->c));
    mutex_init(&(ret->lk));
    return ret;
}

void mq_destroy( struct mq_t* mq ) {
    clearqueue(&(mq->q));
    mutex_delete(&(mq->lk));
    cond_delete(&(mq->c));
    free(mq);
}
