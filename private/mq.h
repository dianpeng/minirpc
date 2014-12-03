#ifndef MQ_H_
#define MQ_H_

/* A THREAD SAFE message queue implementation. This implementation is used to
 * decouple the mini-rpc core service from the external service provider here. */

struct mq;
struct mq* mq_create();
void mq_destroy( struct mq* );
void mq_enqueue( struct mq* , void* data );

/* this function will wake up _all_ thread that is WAITING on the queue */
void mq_wakeup( struct mq* );

/* return 0 --> has one element returned
 * return -1 --> empty queue */
void mq_dequeue( struct mq* , void** data );
int mq_try_dequeue( struct mq* , void** data );

#endif /* MQ_H_ */
