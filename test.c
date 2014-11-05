#include "private/coder.h"
#include "minirpc.h"
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <stdlib.h>
#include <time.h>

#define MAX_PER_THREAD 1
#define MAX_THREADS 64

#ifdef _WIN32
#include <windows.h>
#include <process.h>
typedef HANDLE th_t;
#else
#include <pthread.h>
typedef pthread_t th_t;
#endif /* _WIN32 */

void test_body() {
    int ret;
    struct mrpc_response_t res;
    int i;
    for( i = 0 ; i < MAX_PER_THREAD ; ++i ) {
        ret = mrpc_request( "127.0.0.1:12345" , MRPC_FUNCTION , "Add" , &res , "%u%u" , 1 , 3 );
        assert( ret == 0 );
        assert( res.result.value.uinteger == 4 );
    }
}

#ifdef _WIN32
unsigned int __stdcall simple_pressure_test( void* para ) {
    test_body();
    return 0;
}
#else
void* simple_pressure_test( void* para ) {
    test_body();
    return NULL;
}
#endif /* _WIN32 */

int main() {
    int i;
    th_t* ths;
    clock_t start,end;
    ths = malloc( MAX_THREADS * sizeof(th_t) );
    assert( ths );

    start = clock();
    for( i = 0 ; i < MAX_THREADS ; ++i ) {
#ifdef _WIN32
        ths[i] = (th_t)_beginthreadex( NULL , 0 , simple_pressure_test , NULL , 0 , NULL );
        assert( ths[i] != INVALID_HANDLE_VALUE );
#else
        assert( pthread_create(ths+i,NULL,simple_pressure_test,NULL) == 0 );
#endif /* _WIN32 */
    }

    /* join */
    for( i = 0 ; i < MAX_THREADS ; ++i ) {
#ifdef _WIN32
        if( WaitForSingleObject(ths[i],INFINITE) != WAIT_OBJECT_0 )
            assert(0);
#else
        assert( pthread_join(this[i],NULL) == 0 );
#endif /* _WIN32 */
    }

    end = clock();

    printf("Rough time:%d\n",(end-start)/CLOCKS_PER_SEC);

    return 0;
}

