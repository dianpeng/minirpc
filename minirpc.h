#ifndef MINIRPC_H_
#define MINIRPC_H_
#include <stddef.h>
#include <stdint.h>

/* Protocol for Mini-RPC */
#define MRPC_MAX_LOCAL_VAR_CHAR_LEN 16
#define MRPC_MAX_METHOD_NAME_LEN 128 /* cannot be larger than 254 */
#define MRPC_MAX_PARAMETER_SIZE 16
#define MRPC_MAX_RESULT_SIZE 16

#define MRPC_DEFAULT_TIMEOUT_CLOSE 15000
#define MRPC_DEFAULT_POLL_TIMEOUT 500
#define MRPC_DEFAULT_OUTBAND_SIZE 100
#define MRPC_DEFAULT_RESERVE_MEMPOOL 50

/* method type */
enum {
    MRPC_FUNCTION = 1,
    MRPC_NOTIFICATION = 2
};

/* Supported type comes here */
enum {
    MRPC_UINT = 1,
    MRPC_INT,
    MRPC_VARCHAR
};

/* Varchar */
struct varchar_t {
    const char* val; /* null terminated guaranteed */
    size_t len;
    char buf[MRPC_MAX_LOCAL_VAR_CHAR_LEN];
};

/* Using this function to compose a varchar 
 * struct varchar_t varc; 
 * mrpc_varchar(&varc,"SomeString",0); 
 * own == 1 --> copy
 * own == 0 --> move  */

void mrpc_varchar_create( struct varchar_t* , const char* str , int own );
void mrpc_varchar_destroy( struct varchar_t* );

/* Value */
struct val_t {
    int type;
    union {
        double real;
        uint32_t uinteger;
        int32_t integer;
        struct varchar_t varchar;
    } value;
};

#define mrpc_val_int(val,i) \
    do { \
        (val)->type = MRPC_INT; \
        (val)->value.integer=i; \
    } while(0)

#define mrpc_val_uint(val,i) \
    do { \
        (val)->type = MRPC_UINT; \
        (val)->value.uinteger=i; \
    } while(0)

#define mrpc_val_varchar(val,str,own) \
    do { \
        (val)->type = MRPC_VARCHAR; \
        mrpc_varchar_create(&((val)->value.varchar),(str),own); \
    } while(0)

#define mrpc_val_destroy(val) \
    do { \
        if((val)->type==MRPC_VARCHAR) \
            mrpc_varchar_destroy(&((val)->value.varchar)); \
    }while(0)

/* Protocol result */
struct mrpc_request_t {
    char method_name[MRPC_MAX_METHOD_NAME_LEN];
    size_t method_name_len;
    int method_type;
    char transaction_id[4];
    size_t length;
    size_t par_size;
    struct val_t par[MRPC_MAX_PARAMETER_SIZE];
};

struct mrpc_response_t {
    int method_type;
    char method_name[MRPC_MAX_METHOD_NAME_LEN];
    size_t method_name_len;
    size_t length;
    char transaction_id[4];
    struct val_t result;
    int error_code;
};

/* parsing status */
enum {
    MRPC_REQUEST_PARSE_ERR_OK = 0,
    MRPC_REQUEST_PARSE_ERR_METHOD_TYPE_UNKNOWN = 1 ,
    MRPC_REQUEST_PARSE_ERR_DATA_LEN_INVALID = 2 ,
    MRPC_REQUEST_PARSE_ERR_METHOD_NAME_LENGTH = 3,
    MRPC_REQUEST_PARSE_ERR_PARAMETER_INVALID = 4,
    MRPC_REQUEST_PARSE_ERR_TOO_MANY_PARAMETERS = 5,
    MRPC_REQUEST_PARSE_ERR_PACKAGE_BROKEN = 10
};

/* initialize the mini-rpc */
void mrpc_init();

/* ----------------------------------------
/* Server side
/* --------------------------------------*/
struct minirpc_t;
struct minirpc_t* mrpc_create( const char* logf_name , const char* addr );
int mrpc_run( struct minirpc_t * );
int mrpc_poll( struct minirpc_t * );

int mrpc_request_recv( struct minirpc_t* , struct mrpc_request_t* req , void** );
void mrpc_response_send( struct minirpc_t* , const struct mrpc_request_t* req , void* , const struct val_t* result , int ec );
// This function is used to finish a indication request
void mrpc_response_done( struct minirpc_t* , void* );
void mrpc_write_log( struct minirpc_t* rpc , const char* fmt , ... );

/* ----------------------------------------
/* Client side
/* --------------------------------------*/
/* mini RPC request function
 * blocking version API */
int mrpc_request( const char* addr, int method_type , const char* method_name ,
                  struct mrpc_response_t* res , const char* par_fmt , ... );

/* this function is used to serialize the data into the buffer. the returned value is
 * malloced on heap, after sending it, the user needs to call free function to free it */
void* mrpc_request_serialize( size_t* len , int method_type, const char* method_name , const char* par_fmt, ... );

/* deserialize the input data into a struct mrpc_response_t object */
int mrpc_response_parse( void* buf , size_t sz , struct mrpc_response_t* r );

/* ----------------------------------------
/* Utility Function
/* --------------------------------------*/

/* this function is used for choking out the size of a specific package. Suppose that
 * you get some data from the network, then you want to figure out that if a full package
 * is received, typically you should check this with size of the expected package. The
 * wire format do encapsulate the wire format length for the package, however, we need
 * to parse it in the partial stream and then do the job. This function is provided to
 * extract the size of package within the partial data. Since no matter it is a request,
 * or a response, the first 4 field (header) are the same. This only function is used to
 * extract out the size of that message package. You could put into any length package and
 * it will return the size of that package or it cannot do so */

int mrpc_get_package_size( void* buf , size_t sz , size_t* len );

#endif /* MINIRPC_H_ */