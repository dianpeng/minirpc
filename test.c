#include "private/coder.h"
#include "minirpc.h"
#include <stdio.h>
#include <assert.h>
#include <limits.h>

#if 0

int test_coder() {
    size_t sz = 18;
    char buf[9];
    int ret;
    ret = encode_size(sz,buf,9);
    assert( decode_size(&sz,buf,ret) > 0 );
    assert( sz == 18 );
    return 0;
}

int main() {
    test_coder();
    return 0;
}

#else

int main() {
    int ret;
    struct mrpc_response_t res;

    mrpc_init();
    ret = mrpc_request( "127.0.0.1:12345" , MRPC_FUNCTION , "Hello World" , &res , "" );
    assert(ret == 0);
    printf("%s",res.result.value.varchar.val);
    return 0;
}
#endif
