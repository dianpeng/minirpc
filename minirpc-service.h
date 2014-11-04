#ifndef MINIRPC_SERVICE_H_
#define MINIRPC_SERVICE_H_
#include <stddef.h>

/*
 * This is a very simple C module for the minirpc service.
 * It contains a simple thread pool implementation for
 * executing different service. The user could use it as
 * default way to handle the minirpc service. This module
 * can also initialize as a single thread configuration.
 * It means you could use a single thread to handle MRPC
 * and also the service. See test/exampe for more detail
 */

struct minirpc_t;
struct mrpc_val_t;
struct mrpc_service_t;
struct mrpc_request_t;

struct mrpc_service_t* mrpc_service_create( struct minirpc_t* mrpc , size_t sz ,
    size_t min_slp_time , size_t max_slp_time , void* opaque );

/* typically you don't have to call this function */
void mrpc_service_destroy( struct mrpc_service_t* );

typedef void (*mrpc_service_cb)( struct mrpc_service_t* ,
                                 const struct mrpc_request_t* ,
                                 void*,
                                 int* ,
                                 struct mrpc_val_t* );

/* This function is _NOT_ thread safe, call it before you call mrpc_service_run_remote
 * and after you call mrpc_service_create in the same thread */

int mrpc_service_add( struct mrpc_service_t*, mrpc_service_cb cb , const char* method_name , void* udata );

/* Running the service in the caller thread  */
void mrpc_service_run_once( struct mrpc_service_t* );
void mrpc_service_run( struct mrpc_service_t* );

/* Running the service in the remote thread  */
int mrpc_service_run_remote( struct mrpc_service_t* , int thread_sz );
/* Call this function inside of the thread that call mrpc_service_run_remote
 * This function will block until all the thread join in the calling thread */
int mrpc_service_quit( struct mrpc_service_t* );

/* utility */
void* mrpc_service_get_udata( struct mrpc_service_t* );
struct minirpc_t* mrpc_service_get_mrpc( struct mrpc_service_t* );

#endif /* MINIRPC_SERVICE_H_ */
