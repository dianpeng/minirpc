#ifndef MINIRPC_SERVICE_H_
#define MINIRPC_SERVICE_H_
#include <stddef.h>

/*
 * This is the default Mini RPC service module. A service means a reigetered
 * RPC routine. This service module provides infrastructure to register,execute
 * service using multi-thread. To make it more flexible, the user is able to
 * call the service execution manually as well, and it means user could multiplex
 * the service execution with MRPC poll function in a single thread when thread
 * is not available
 */

struct mrpc_val;
struct mrpc_service;
struct mrpc_request;

struct mrpc_service* mrpc_service_create( size_t sz ,
    size_t min_slp_time , size_t max_slp_time , void* opaque );

/* Typically you don't have to call this function */
void mrpc_service_destroy( struct mrpc_service* );

typedef void (*mrpc_service_cb)( struct mrpc_service* ,
                                 const struct mrpc_request* ,
                                 void*,
                                 int* ,
                                 struct mrpc_val* );

/* This function is _NOT_ thread safe, call it before you call mrpc_service_run_remote
 * and after you call mrpc_service_create in the same thread */

int mrpc_service_add( struct mrpc_service*, mrpc_service_cb cb , const char* method_name , void* udata );

/* Running the service in the caller thread  */
void mrpc_service_run_once( struct mrpc_service* );
void mrpc_service_run( struct mrpc_service* );

/* Running the service in the remote thread, the user needs to 
 * specify the thread number. This function will not block or 
 * run any callback function inside of it. All the callback 
 * function will be executed in back ground thread.
 */
 
int mrpc_service_run_remote( struct mrpc_service* , int thread_sz );

/* Call this function inside of the thread that call mrpc_service_run_remote
 * This function will block until all the thread join in the calling thread.
 * This function may deadlock if the user doesn't call mrpc_interrupt, the
 * service module will not call this function manually. A typical way is just
 * run the mrpc_run in main thread , and put this function right after it. 
 * It is because MRPC take care of signal handling, so a Ctrl+Z/C/X can be 
 * issued from user to _STOP_ the service , after that the mrpc_run will exit
 * automatically and service module _DO_ receive this interruption as well 
 * internally . Then call mrpc_service_run_quit after mrpc_run is _ALWAYS_
 * safe there. */
 
int mrpc_service_quit( struct mrpc_service* );

/* Utility */

/* Get opaque user data */
void* mrpc_service_get_udata( struct mrpc_service* );

#endif /* MINIRPC_SERVICE_H_ */
