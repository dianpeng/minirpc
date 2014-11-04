#include <minirpc.h>
#include <stdio.h>

// We will simulate a Hello World function

static
void HelloWorld_CB(
    const struct mrpc_request_t* req ,
    void* key,
    struct minirpc_t* rpc ) {
    struct val_t result;
    mrpc_val_varchar(&result,"Hello World",0);
    mrpc_response_send(rpc,req,key,&result,0);
    mrpc_val_destroy(&result);
}

int main() {
    struct minirpc_t* rpc ;

    mrpc_init();
    rpc = mrpc_create("log.txt","127.0.0.1:12345");
    if( rpc == NULL ) {
        fprintf(stderr,"cannot create minirpc\n");
        return -1;
    }

    while(1) {
        int ret = mrpc_poll(rpc);
        void* data;
        struct mrpc_request_t req;
        if( ret < 0 ) {
            fprintf(stderr,"minirpc-error\n");
        }
        if( mrpc_request_recv(rpc,&req,&data) == 0 ) {
            HelloWorld_CB(&req,data,rpc);
        }
    }
}