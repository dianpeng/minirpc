#include "mq.h"
#include "conf.h"

#include <string.h>
#include <stdlib.h>

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
void mutex_delete( mutex_t* l ) {
    DeleteCriticalSection(l);
}

/*
 * Condition variable on Windows. We don't provide implementation
 * for supporting less than Windows XP version. Use native CV is
 * fine here
 */

typedef CONDITION_VARIABLE cond_t;
static
void cond_init( cond_t* c ) {
    InitializeConditionVariable(c);
}

static
int cond_wait( cond_t* c , mutex_t* m , int msec ) {
    BOOL bret = SleepConditionVariableCS(c,m, msec < 0 ? INFINITE : (DWORD)(msec));
    return bret == TRUE ? 0 : -1;
}

static
void cond_signal_one( cond_t* c  ) {
    WakeConditionVariable(c);
}

static
void cond_signal_all( cond_t* c ) {
    WakeAllConditionVariable(c);
}

static
void cond_delete( cond_t* c ) {
    c=c;
}

#ifdef _MSC_VER
typedef volatile LONG spinlock_t;
#else
typedef int spinlock_t;
#endif /* _MSC_VER */
static
void spinlock_init( spinlock_t* lk ) {
    *lk = 0;
}

static
void spinlock_lock( spinlock_t* lk ) {
#ifdef _MSC_VER
    /* Acquire semantic */
    while( InterlockedCompareExchangeAcquire(
        CAST(LONG volatile*,lk),
        1,
        0 ) != 0 );
#else
    /* Full memory barrier */
    while( __sync_val_compare_and_swap(lk,0,1) != 0 );
#endif /* _MSC_VER */
}

static
void spinlock_unlock( spinlock_t* lk ) {
#ifdef _MSC_VER
#ifndef NDEBUG
    LONG ret =
#endif /* NDEBUG */
    /* Release semantic */
    InterlockedCompareExchangeRelease(CAST(LONG volatile*,lk),0,1);
    assert( ret == 1 );
#else
#ifndef NDEBUG
    int ret =
#endif /* NDEBUG */
    __sync_val_compare_and_swap(lk,1,0);
    assert( ret == 1 );
#endif /* _MSC_VER */
}

static
void spinlock_delete( spinlock_t* lk ) {
#ifndef NDEBUG
#ifdef _MSC_VER
    MemoryBarrier();
#else
    __sync_synchronize();
#endif /* NDEBUG */
    assert( *lk == 0 );
#endif /* NDEBUG */
    lk = lk;
}

#else
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
typedef pthread_cond_t cond_t;

static
void mutex_init( mutex_t* m ) {

#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_mutex_init(m,NULL);
    assert( ret == 0 );

}

static
void mutex_lock( mutex_t* m ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_mutex_lock(m);
    assert( ret == 0 );

}

static
void mutex_unlock( mutex_t* m ) {
    pthread_mutex_unlock(m);
}

static
void mutex_delete( mutex_t* m ) {
    pthread_mutex_destroy(m);
}

static
void cond_init( cond_t* c ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_cond_init(c,NULL);
    assert( ret == 0 );
}

static
int cond_wait( cond_t* c , mutex_t* l , int msec ) {
    struct timespec tv;
    int ret;
    if( msec != -1 ) {
        tv.tv_sec = msec/1000;
        tv.tv_nsec = (msec%1000)*1000;
    }
    ret = pthread_cond_timedwait(c,l,&tv);
    return ret == 0 ? 0 : -1;
}

static
void cond_signal_one( cond_t* c ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_cond_signal(c);
    assert( ret == 0 );
}

static
void cond_signal_all( cond_t* c ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_cond_broadcast(c);
    assert( ret == 0 );
}

static
void cond_delete( cond_t* c ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_cond_destroy(c);
    assert( ret == 0 );
}

/* raw spin lock */
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

static
void spinlock_delete( spinlock_t* lk ) {
#ifndef NDEBUG
int ret =
#endif /* NDEBUG */
    pthread_spin_destroy(lk);
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
};

static
void initqueue ( struct queue_t* q ) {
    q->tail.next = &(q->tail);
    q->tail.prev = &(q->tail);
    q->tail.data = NULL;
}

static
void enqueue( struct queue_t* q , struct queue_node_t* n ) {
    q->tail.prev->next = n;
    n->prev = q->tail.prev;
    n->next = &(q->tail);
    q->tail.prev = n;
}

static
int dequeue( struct queue_t* q , struct queue_node_t** n ) {
   if( q->tail.next == &(q->tail) )
       return -1;
   *n = q->tail.next;
   q->tail.next = (*n)->next;
   (*n)->next->prev = &(q->tail);
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
    struct queue_t q; /* the real queue */
    spinlock_t sp_lk; /* spin lock to protect the queue itself */
    mutex_t lk;       /* this mutex and condition variable is used to protect sleeped thread */
    cond_t c;
    int sleep_thread; /* only when no work is there, it will be useful */
    int exit; /* this flag is used to notify the blocked the dequeue function to exit */
};

struct mq_t* mq_create() {
    struct mq_t* ret = malloc( sizeof(*ret) );
    VERIFY(ret);
    initqueue(&(ret->q));
    cond_init(&(ret->c));
    mutex_init(&(ret->lk));
    spinlock_init(&(ret->sp_lk));
    ret->sleep_thread = 0;
    ret->exit = 0;
    return ret;
}

void mq_destroy( struct mq_t* mq ) {
    clearqueue(&(mq->q));
    mutex_delete(&(mq->lk));
    cond_delete(&(mq->c));
    spinlock_delete(&(mq->sp_lk));
    free(mq);
}

void mq_enqueue( struct mq_t* mq , void* data ) {
    /* do the allocation */
    struct queue_node_t* n = malloc(sizeof(*n));
    VERIFY(n);
    assert( data );

    n->data = data;
    /* the queue itself is protected by the spinlock */
    spinlock_lock(&(mq->sp_lk));
    enqueue(&(mq->q),n);
    spinlock_unlock(&(mq->sp_lk));
    /* check if there're sleeped thread then we need to wake them up */
    if( mq->sleep_thread != 0 ) {
        /* this may lead to the target thread lose the wake up
         * however we fix them by letting target thread using
         * timed wake up instead of sleeping permanently, the
         * reason that we don't use pthread_mutex is to avoid
         * contention here. Currently the mq_enqueue is nearly a
         * lock-free data structure(not wait free). */
        cond_signal_one(&(mq->c));
    }
}


#define MAX_SPIN 10

/* We use an adaptive algorithm to adjust sleep time */
#define MIN_SLEEP_TIME 2
#define MAX_SLEEP_TIME 256

void mq_dequeue( struct mq_t* mq, void** data ) {
    /* get data from the back queue no lock now. */
    struct queue_node_t* n=NULL;
    int ret;

    /* try to dequeue the data from the queue */
    spinlock_lock(&(mq->sp_lk));
    ret = dequeue(&(mq->q),&n);
    spinlock_unlock(&(mq->sp_lk));

    if( ret != 0 ) {
        /* When we reach here , it means that we don't have any data in queue
         * to avoid frequent sleep, here we will first try to spin some time
         * and then falling into the sleep */
        int i = MAX_SPIN;
        int slp_time = MIN_SLEEP_TIME;

        /* Busy spin here to avoid early sleep.
         * Is it useful ? */
        while( i-- && ret != 0 && !mq->exit ) {
            spinlock_lock(&(mq->sp_lk));
            ret = dequeue(&(mq->q),&n);
            spinlock_unlock(&(mq->sp_lk));
        }

        /* check if we have that luck */
        if( ret == 0 || mq->exit )
            goto done;

        /* now we need to wait for the condition now,
         * since we may loose the signal , so put it
         * into the loop here */
        mutex_lock(&(mq->lk));
        ++mq->sleep_thread;
        do {
            cond_wait(&(mq->c),&(mq->lk),slp_time);
            /* Exponential avoidance for busy polling */
            slp_time *= 2;
            if ( slp_time > MAX_SLEEP_TIME )
                slp_time = MAX_SLEEP_TIME;

            /* This spinlock will _NOT_ blocked by other thread or cause very
             * long waiting. Dequeue is supposed to have simple instructions.
             * (1) The mq_enqueue operation will _NOT_ hold mutex, so no double
             * lock at same time.
             * (2) The other mq_dequeue will _ONLY_ wait spin lock when it gets the
             * mutex, so after that, no other mq_dequeue can has spin lock. However,
             * the mq_enqueue do can have. It is OK, since again, it is used to protect
             * dequeue operation which is very simple  */

            spinlock_lock(&(mq->sp_lk));
            ret = dequeue(&(mq->q),&n);
            spinlock_unlock(&(mq->sp_lk));

        } while( ret != 0 && !mq->exit );

        /* When we reach here, it means that we have already get the
         * new data in the queue. Therefore, we can unlock the mutex
         * and return what we have now */
        --mq->sleep_thread;
        mutex_unlock(&(mq->lk));
    }
done:
    /* We get what we want */
    if( mq->exit ) {
        *data = NULL;
    } else {
        *data = n->data;
    }
    free(n);
}

int mq_try_dequeue( struct mq_t* mq , void** data ) {
    struct queue_node_t* n;
    int ret;

    if( mq->exit ) {
        *data = NULL;
        return 0;
    }

    spinlock_lock(&(mq->sp_lk));
    ret = dequeue(&(mq->q),&n);
    spinlock_unlock(&(mq->sp_lk));

    if( ret == 0 ) {
        *data = n->data;
        free(n);
        return 0;

    } else {
        return -1;
    }
}

void mq_wakeup( struct mq_t* mq ) {
    mq->exit = 1;
    cond_signal_all(&(mq->c));
}
