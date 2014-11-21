#include "../minirpc.h"
#include "../minirpc-service.h"
#include <assert.h>
#include <string.h>
#include <stdio.h>

/*
 * Using MiniRPC Service module to help us handle the business
 * logic in backend thread.
 */

static
void
hello_world_cb( struct mrpc_service_t* service ,
                const struct mrpc_request* req ,
                void* udata,
                int* error_code,
                struct mrpc_val* val ) {

    assert(strcmp(req->method_name,"Hello World") == 0);
    /* checking the parameter size */
    if( req->par_size != 0 ) {
        *error_code = MRPC_EC_FUNCTION_INVALID_PARAMETER_SIZE;
        return;
    }
    /* formating the return value */
    mrpc_val_varchar(val,"Hello World",0);
    /* setting the correct error code */
    *error_code = MRPC_EC_OK;
}

/* This function will calculate the addition of 2 unsigned integer */
static
void
addition_cb( struct mrpc_service_t* service ,
             const struct mrpc_request* req ,
             void* udata,
             int* error_code ,
             struct mrpc_val* val ) {

    if( req->par_size != 2 ) {
        *error_code = MRPC_EC_FUNCTION_INVALID_PARAMETER_SIZE;
        return;
    }
    /* get the 2 unsigned integer value */
    if( req->par[0].type != MRPC_UINT || req->par[1].type != MRPC_UINT ) {
        *error_code = MRPC_EC_FUNCTION_INVALID_PARAMETER_TYPE;
        return;
    }
    /* do the calculation here */
    mrpc_val_uint(val,req->par[0].value.uinteger+req->par[1].value.uinteger);
    /* setting the correct error code */
    *error_code = MRPC_EC_OK;
}

int main() {
    struct mrpc_service* service;
    int ret;

    if( mrpc_init("log.txt","127.0.0.1:12345",0) != 0 ) {
        fprintf(stderr,"Cannot initialize MRPC!");
        return -1;
    }

    service = mrpc_service_create(128,0,50,NULL);

    /* register the service entry */
    mrpc_service_add( service, hello_world_cb, "Hello World", NULL );
    mrpc_service_add( service, addition_cb, "Add", NULL );

    /* start the service in backend thread */
    mrpc_service_run_remote(service,12);

    /* start working now */
    ret = mrpc_run();

    if( ret < 0 ) {
        /* Handle error */
        fprintf(stderr,"MRPC Error!");
    } else {
        /* User interruption */
        fprintf(stderr,"User interruption!");
    }

    /* (1) Quit the service (2) Stop the MRPC */
    mrpc_service_quit(service);
    mrpc_clean();

    return 0;
}
