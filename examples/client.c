#include "private/coder.h"
#include "minirpc.h"
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#ifdef _WIN32
#include <Windows.h>
#include <process.h>
#else
#include <pthread.h>
#endif /* _WIN32 */


#define MAX_REQUEST 512
int TIMES = MAX_REQUEST;
clock_t START;

#define VERIFY(x) \
    do { \
        if(!(x)) { \
            abort(); \
        } \
    } while(0)

static
void mrpc_req_cb( const struct mrpc_response_t* res , void* data ) {
    VERIFY( res != NULL );
    VERIFY( res->result.value.uinteger == 4 );
    --TIMES;
    printf(".");
    if( TIMES == 0 ) {
        printf("Time:%d\n",clock()-START);
        mrpc_interrupt();
    }
}

#ifdef _WIN32
static unsigned int __stdcall
#else
static
void*
#endif /* _WIN32 */
thread_main(void* par) {
    int i;
    for( i = 0 ; i < MAX_REQUEST ; ++i ) {
        VERIFY ( mrpc_request_async(
                    mrpc_req_cb,
                    NULL,
                    5000,
                    "127.0.0.1:12345",
                    MRPC_FUNCTION,
                    "Add",
                    "%u%u",
                    1,3) == 0 );
    }
#ifdef _WIN32
    START = clock();
    return 0;
#else
    return NULL;
#endif /* _WIN32 */
}


int main() {
    /* initialize another thread to issue the request */
#ifdef _WIN32
    HANDLE hThread;
    mrpc_init("log.txt","127.0.0.1:12346",1);
    hThread = (HANDLE)_beginthreadex(
        NULL,0,thread_main,NULL,0,NULL);
    WaitForSingleObject(hThread,INFINITE);
#else
    pthread_t th;
    mrpc_init("log.txt","127.0.0.1:12346",1);
    pthread_create(&th,NULL,_thread_main,NULL);
    pthread_join(th,NULL);
#endif /* _WIN32 */
    mrpc_run();
    return 0;
}




