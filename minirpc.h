#ifndef MINIRPC_H_
#define MINIRPC_H_
#include <stddef.h>
#include <stdint.h>

/* The following macro is used to define rpc specific configuration.
 * The user could change it to adapt its requirement */
 
#define MRPC_MAX_LOCAL_VAR_CHAR_LEN 16 /* Small string optimization length */
#define MRPC_MAX_METHOD_NAME_LEN 128   /* The max method name supported */
#define MRPC_MAX_PARAMETER_SIZE 16     /* The max parameter size for a function  call */

#define MRPC_DEFAULT_TIMEOUT_CLOSE 15000 /* The default time out close for server */
#define MRPC_DEFAULT_OUTBAND_SIZE 100    /* The default number of how many data is allowed to send out outstanding */
#define MRPC_DEFAULT_RESERVE_MEMPOOL 50  /* The default memory pool initial size */

/* Method type */
enum {
    MRPC_FUNCTION = 1, /* Function represent a Request/Reply model */
    MRPC_NOTIFICATION = 2 /* Notification represent a one way message send */
};

/* Supported type comes here */
enum {
    MRPC_UINT = 1,
    MRPC_INT,
    MRPC_VARCHAR
};

/* Varchar 
 * The varchar has a simple small string optimization here, if a string is less than
 * MRPC_MAX_LOCAL_VAR_CHAR_LEN, it will store _locally_. A string is always guarranted
 * to be a null-terminated string 
 */
 
struct mrpc_varchar_t {
    const char* val; /* null terminated guaranteed */
    size_t len;
    char buf[MRPC_MAX_LOCAL_VAR_CHAR_LEN];
};

/* Using this function to compose a varchar
 * struct mrpc_varchar_t varc;
 * mrpc_varchar(&varc,"SomeString",0);
 * own == 1 --> copy
 * own == 0 --> move  */

void mrpc_varchar_create( struct mrpc_varchar_t* , const char* str , int own );
void mrpc_varchar_destroy( struct mrpc_varchar_t* );

/* Value 
 * Using the type to check what exactl value stored inside of the union value object
 * Type is always one of MRPC_UINT/MRPC_INT/MRPC_VARCHAR
 */
 
struct mrpc_val_t {
    int type;
    union {
        uint32_t uinteger;
        int32_t integer;
        struct mrpc_varchar_t varchar;
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

/* Protocol part */

/* A request object, it represents a specific request on the wire */
struct mrpc_request_t {
    char method_name[MRPC_MAX_METHOD_NAME_LEN];
    size_t method_name_len;
    int method_type;
    char transaction_id[4];
    size_t length;
    size_t par_size;
    struct mrpc_val_t par[MRPC_MAX_PARAMETER_SIZE];
};

/* A response object, it represents a response object from the peer */
struct mrpc_response_t {
    int method_type;
    char method_name[MRPC_MAX_METHOD_NAME_LEN];
    size_t method_name_len;
    size_t length;
    char transaction_id[4];
    struct mrpc_val_t result;
    int error_code;
};

/* error code
 *
 * The error code section is inside of the response object. If the error code
 * is not MRPC_OK , then the user should not expect any result field in that
 * response object. The error code is used to convey the external error , so
 * a function execution error is not counted here. You could treat this error
 * as something like , stack layout error or parameter size error. Which is
 * not the error like I cannot open a file since the file is not existed. A
 * simple way to judge it is without ever executing the real logic of a function,
 * that error is still generated.
 *
 */

enum {
    MRPC_EC_OK = 0,
    MRPC_EC_FUNCTION_NOT_FOUND,
    MRPC_EC_FUNCTION_INVALID_PARAMETER_SIZE,
    MRPC_EC_FUNCTION_INVALID_PARAMETER_TYPE
};

/* Initialize the mini-rpc */
int mrpc_init( const char* logf_name , const char* addr , int polling_time );

/* Clean the MRPC, it could be optional if after stop MRPC, you will exit the process */
void mrpc_clean();

/* ----------------------------------------
 * Server side
 * --------------------------------------*/
 
 /* Run means run it until the interrupt called or error (cannot recover) happened */
int mrpc_run();

/* Poll means run once, you could use this function to multiplex the service and rpc IO
 * in a single thread , but using a loop and call each function once.
 * Eg:
 * for (;;) {
 *    mrpc_poll();
 *    mrpc_service_run_once();
 * }
 */
 
int mrpc_poll();

/* Interrupt all blocking async operation of MRPC 
 * After, interruption , any call to MRPC function
 * is undefined. MRPC has already taken care of signal 
 */
void mrpc_interrupt();

/* All the following functions are thread safe */

/* The mrpc_request_(try)_recv function is used to grab the data from the queue 
 * The *try* version will not block and just try to get the data. The other one
 * will block until a data is there .
 * However, due to user calls mrpc_interrupt, this function may still return without
 * any data available.
 * Return 0 : success ; return 1 : interruption ; return -1: failed. 
 */
int mrpc_request_try_recv( struct mrpc_request_t* req , void** );
int mrpc_request_recv( struct mrpc_request_t* req , void** );

void mrpc_response_send( const struct mrpc_request_t* req , void* , const struct mrpc_val_t* result , int ec );

/* This function is used to finish a indication request */
void mrpc_response_done( void* );

/* Writing the log into the MRPC server log file */
void mrpc_write_log( const char* fmt , ... );

/* ----------------------------------------
 * Client side
 * --------------------------------------*/

/* Mini RPC request function and it is the blocking version API */
int mrpc_request( const char* addr, int method_type , const char* method_name ,
                  struct mrpc_response_t* res , const char* par_fmt , ... );


typedef void (*mrpc_request_async_cb)( const struct mrpc_response_t* res , void* data );

/* Mini RPC request function with non blocking version API.
 * It requires that the MRPC is running now , so it means
 * call it _AFTER_ a certain thread called MRPC_POLL .
 * The callback function will be called in the MRPC_POLL thread */
int mrpc_request_async( mrpc_request_async_cb cb , void* data , int timeout , 
                        const char* addr, int method_type , const char* method_name ,
                        const char* par_fmt , ... );

/* This function is used to serialize the data into the buffer. the returned value is
 * malloced on heap, after sending it, the user needs to call free function to free it */
void* mrpc_request_serialize( size_t* len , int method_type, const char* method_name , const char* par_fmt, ... );

/* Deserialize the input data into a struct mrpc_response_t object */
int mrpc_response_parse( void* buf , size_t sz , struct mrpc_response_t* r );

/* ----------------------------------------
 * Utility Function
 * --------------------------------------*/

/* This function is used for choking out the size of a specific package. Suppose that
 * you get some data from the network, then you want to figure out that if a full package
 * is received, typically you should check this with size of the expected package. The
 * wire format do encapsulate the wire format length for the package, however, we need
 * to parse it in the partial stream and then do the job. This function is provided to
 * extract the size of package within the partial data. You could feed this function with
 * any length data , once this function figure out how large a package should be , it 
 * will return 0 , then you just need to loop your read until the full package is received
 * and then call with other parse routine. */

int mrpc_get_package_size( void* buf , size_t sz , size_t* len );

#endif /* MINIRPC_H_ */
