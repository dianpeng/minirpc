#include "minirpc.h"
#include "private/coder.h"
#include "private/mq.h"
#include "private/network.h"
#include "private/conf.h"
#include "private/mem.h"

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <errno.h>

/*
 * Wire Protocol
 */

static
int decode_varchar( void* buffer , size_t length , struct val_t* val ) {
    unsigned int str_len;
    int ret;
    if( length < 4 )
        return -1;
    /* Variant byte length : Variant string goes here */
    ret = decode_uint(&str_len,CAST(char*,buffer),length);
    if( ret < 0 )
        return -1;
    buffer=CAST(char*,buffer)+ret;
    val->value.varchar.len = CAST(size_t,str_len);
    /* Checking package completion */
    if( str_len > length - ret )
        return -1;
    if( str_len < MRPC_MAX_LOCAL_VAR_CHAR_LEN ) {
        memcpy(val->value.varchar.buf,buffer,str_len);
        val->value.varchar.buf[str_len] = 0;
        val->value.varchar.val=val->value.varchar.buf;
    } else {
        /* malloc new buffer since we cannot hold it in local buffer */
        val->value.varchar.val = malloc(str_len+1);
        VERIFY(val->value.varchar.val == NULL);
        memcpy(CAST(void*,val->value.varchar.val),buffer,str_len);
        CAST(char*,val->value.varchar.val)[str_len]=0;
    }
    /* variant length + string content length */
    return ret + str_len;
}

static
size_t encode_varchar( const struct varchar_t* varchar , char* buffer ) {
    int ret = encode_uint(varchar->len,buffer);
    buffer=CAST(char*,buffer)+ret;
    memcpy(buffer,varchar->val,varchar->len);
    return ret+varchar->len;
}

static
size_t mrpc_cal_val_size( const struct val_t* val ) {
    switch(val->type) {
    case MRPC_UINT:
        return 1+encode_size_uint(val->value.uinteger);
    case MRPC_INT:
        return 1+encode_size_int(val->value.integer);
    case MRPC_VARCHAR:
        return 1+val->value.varchar.len+
            encode_size_uint(val->value.varchar.len);
    default: assert(0); return 0;
    }
}

/* The input buffer must be ensured to be large enough */
static
int mrpc_encode_val( const struct val_t* val , char* buffer ) {
    *CAST(char*,buffer) = val->type;
    buffer=CAST(char*,buffer)+1;

    switch(val->type) {
    case MRPC_UINT:
        return encode_uint(val->value.uinteger,buffer);
    case MRPC_INT:
        return encode_int(val->value.integer,buffer);
    case MRPC_VARCHAR:
        return encode_varchar(&(val->value.varchar),buffer);
    default: assert(0); return 0;
    }
}

static
int mrpc_decode_val( struct val_t* val , const char* buffer , size_t length ) {
    int type;
    int ret;
    if( length < 1 )
        return -1;

    type = *CAST(char*,buffer);

    val->type = type;
    switch(type) {
    case MRPC_UINT:
        ret = decode_int( &(val->value.integer) , CAST(char*,buffer)+1 , length -1 );
        if( ret <0 )
            return ret;
        return ret + 1;
    case MRPC_INT:
        ret = decode_uint( &(val->value.uinteger) , CAST(char*,buffer)+1 , length - 1 );
        if( ret < 0 )
            return ret;
        return ret + 1;
    case MRPC_VARCHAR:
        ret = decode_varchar( CAST(char*,buffer)+1 , length-1, val );
        if( ret < 0 )
            return ret;
        return ret+1;
    default: return -1;
    }
}

#define ENSURE(x,l) \
    do {  \
        if(l<x) \
            return -1; \
    } while(0)

/*
 * The method message length can be 1 byte , 5 byte
 * and 9 bytes which means it contains really large
 */

static
int mrpc_request_parse( void* buffer , size_t length , struct mrpc_request_t* req ) {
    size_t len;
    size_t cur_pos = 0;
    int ret;

    /* method type */
    ENSURE(1,length-cur_pos);
    req->method_type = *CAST(char*,buffer);
    if( req->method_type != MRPC_NOTIFICATION && req->method_type != MRPC_FUNCTION )
        return MRPC_REQUEST_PARSE_ERR_METHOD_TYPE_UNKNOWN;
    buffer=CAST(char*,buffer)+1;
    ++cur_pos;

    /* method length */
#ifndef NDEBUG
    ret=decode_size(&(req->length),CAST(char*,buffer),CAST(size_t,length)-cur_pos);
    if( ret < 0 || req->length == 0 )
        return MRPC_REQUEST_PARSE_ERR_DATA_LEN_INVALID;
    assert( length = req->length );
#else
    req->length = length;
#endif /* NDEBUG */
    buffer=CAST(char*,buffer)+ret;
    cur_pos += ret;

    /* transaction id */
    ENSURE(4,length-cur_pos);
    req->transaction_id[0] = *CAST(char*,buffer);
    req->transaction_id[1] = *(CAST(char*,buffer)+1);
    req->transaction_id[2] = *(CAST(char*,buffer)+2);
    req->transaction_id[3] = *(CAST(char*,buffer)+3);
    buffer=CAST(char*,buffer)+4;
    cur_pos += 4;

    /* method name, only the prefix is safe to get */
    ENSURE(1,length-cur_pos);
    decode_byte(&len,CAST(char*,buffer));
    if( len >= MRPC_MAX_METHOD_NAME_LEN || len == 0 ) {
        return MRPC_REQUEST_PARSE_ERR_METHOD_NAME_LENGTH;
    }
    buffer=CAST(char*,buffer)+1;
    cur_pos += 1;

    /* check validation from the current position */
    if( length-cur_pos < len ) {
        return MRPC_REQUEST_PARSE_ERR_PACKAGE_BROKEN;
    } else {
        if( len >= MRPC_MAX_METHOD_NAME_LEN ) {
            return MRPC_REQUEST_PARSE_ERR_METHOD_NAME_LENGTH;
        }
        ENSURE(len,length-cur_pos);
        memcpy(req->method_name,buffer,len);
        (req->method_name)[len] = 0;
        req->method_name_len = CAST(size_t,len);
    }
    buffer=CAST(char*,buffer)+len;
    cur_pos += len;

    /* parameter list Type(1byte):Value */
    while( cur_pos < length ) {
        int ret;
        req->par_size = 0;
        ret = mrpc_decode_val( &(req->par[req->par_size]) ,
            CAST(char*,buffer) , CAST(size_t,length) - cur_pos );
        if( ret < 0 )
            return MRPC_REQUEST_PARSE_ERR_PARAMETER_INVALID;
        cur_pos += ret;
        ++(req->par_size);
        if( req->par_size == MRPC_MAX_PARAMETER_SIZE )
            return MRPC_REQUEST_PARSE_ERR_TOO_MANY_PARAMETERS;
    }

    return 0;
}

static
size_t mrpc_cal_response_size( const struct mrpc_response_t* response ) {
    /* Method has 1 bytes , since message header length is variable, just leave it here
     * Transaction code has 4 bytes . Method name length has 1 byte and followed by the
     * var length string */
    size_t sz = 1 + 4 + (1 + response->method_name_len);
    /* Error code is variable length */
    sz += encode_size_int(response->error_code);
    /* Result code buffer length */
    sz += mrpc_cal_val_size( &(response->result) );
    /* Lastly method length
     * But we need to consider the truth that we need extra X space to
     * encode the message length _AS_WELL_ , this length should cover
     * everything here. */
    if( encode_size_size(sz+1) == 1 ) {
        return sz+1;
    } else {
        return sz+1+sizeof(size_t);
    }
}

static
void* mrpc_response_serialize( const struct mrpc_response_t* response , size_t* len ) {
    /* Calculate the response buffer length */
    size_t sz = mrpc_cal_response_size(response);
    void* data = malloc(CAST(size_t,sz));
    void* h = data;
    int ret;

    VERIFY(data);
    /* Do the serialization one by one now */
    *CAST(char*,data) = CAST(char,response->method_type);
    data=CAST(char*,data)+1;
    /* Length */
    ret = encode_size(sz,CAST(char*,data),sz-1);
    assert(ret >0);
    data=CAST(char*,data)+ret;
    /* Transaction ID */
    CAST(char*,data)[0] = response->transaction_id[0];
    CAST(char*,data)[1] = response->transaction_id[1];
    CAST(char*,data)[2] = response->transaction_id[2];
    CAST(char*,data)[3] = response->transaction_id[3];
    data=CAST(char*,data)+4;
    /* Error Code */
    ret = encode_int(response->error_code,CAST(char*,data));
    assert( ret > 0 );
    data=CAST(char*,data)+ret;
    /* Method Name */
    encode_byte(CAST(char,response->method_name_len),CAST(char*,data));
    data=CAST(char*,data)+1;
    memcpy(data,response->method_name,response->method_name_len);
    data=CAST(char*,data)+response->method_name_len;
    /* Result */
    ret = mrpc_encode_val( &(response->result), CAST(char*,data) );
    assert(ret >0);
    /* Done */
    *len = sz;
    return h;
}

static
size_t mrpc_cal_request_size( const struct mrpc_request_t* req ) {
    size_t sz = 1 + 4 + (1+req->method_name_len);
    size_t i ;
    for( i = 0 ; i < req->par_size ; ++i ) {
        sz += mrpc_cal_val_size(req->par+i);
    }
    if( encode_size_size(sz+1) == 1 )
        return sz+1;
    else
        return sz+1+sizeof(size_t);
}

static
void* mrpc_request_msg_serialize( const struct mrpc_request_t* req , size_t* len ) {
    size_t sz = mrpc_cal_request_size(req);
    void* data = malloc(sz);
    void* h = data;
    int ret;
    size_t i;

    VERIFY(data);
    *len = sz;

    /* Method type */
    *CAST(char*,data) = req->method_type;
    data=CAST(char*,data)+1;
    --sz;
    /* Length */
    ret=encode_size(*len,data,sz);
    assert( ret >0 );
    data=CAST(char*,data)+ret;
    sz-=ret;
    /* Transaction ID */
    CAST(char*,data)[0]=req->transaction_id[0];
    CAST(char*,data)[1]=req->transaction_id[1];
    CAST(char*,data)[2]=req->transaction_id[2];
    CAST(char*,data)[3]=req->transaction_id[3];
    sz-=4;
    data=CAST(char*,data)+4;
    /* Method name */
    encode_byte(CAST(char,req->method_name_len),CAST(char*,data));
    data=CAST(char*,data)+1;
    memcpy(data,req->method_name,req->method_name_len);
    data=CAST(char*,data)+req->method_name_len;
    sz-=1+req->method_name_len;
    /* Parameters */
    for( i = 0 ; i < req->par_size ; ++i ) {
        ret = mrpc_encode_val(req->par+i,data);
        assert(ret >0);
        data=CAST(char*,data)+ret;
        sz-=ret;
    }
    assert( sz==0 );
    return h;
}

int
mrpc_response_parse( void* data , size_t length , struct mrpc_response_t* response ) {
    int ret;

    /* method type */
    response->method_type=*CAST(char*,data);
    data=CAST(char*,data)+1;
    --length;
    if( response->method_type != MRPC_FUNCTION )
        return -1;

    /* length */
    ret = decode_size(&response->length,data,length);
    if( ret <0 )
        return -1;
    data=CAST(char*,data)+ret;
    length-=ret;

    /* transaction id */
    ENSURE(4,length);
    response->transaction_id[0]=CAST(char*,data)[0];
    response->transaction_id[1]=CAST(char*,data)[1];
    response->transaction_id[0]=CAST(char*,data)[2];
    response->transaction_id[1]=CAST(char*,data)[3];
    data=CAST(char*,data)+4;
    length-=4;

    /* error code */
    ret = decode_int(&response->error_code,data,length);
    if( ret < 0 )
        return -1;
    data=CAST(char*,data)+ret;
    --length;

    /* method */
    ENSURE(1,length);
    decode_byte(&(response->method_name_len),CAST(char*,data));

    if( response->method_name_len >= MRPC_MAX_METHOD_NAME_LEN ||
        response->method_name_len == 0 )
        return -1;
    data=CAST(char*,data)+1;
    ENSURE(response->method_name_len,length);
    memcpy(response->method_name,data,response->method_name_len);
    response->method_name[response->method_name_len]=0;
    data=CAST(char*,data)+response->method_name_len;
    length-=response->method_name_len+1;

    /* result */
    ret = mrpc_decode_val(&response->result,data,length);
    if( ret<0 )
        return -1;
    length -= ret;

    return length == 0 ? 0 : -1;
}

/*
 * MRPC
 */


struct minirpc_t {
    struct mq_t* req_q; /* request queue */
    struct mq_t* res_q; /* response queue */
    struct net_server_t server; /* server for network */
    FILE* logf;
    struct slab_t conn_slab; /* slab for connection */
};

enum {
    PENDING_REQUEST_OR_INDICATION,
    EXECUTE_RPC,
    PENDING_REPLY,
    CONNECTION_FAILED
};

struct mrpc_res_data_t {
    void* buf;
    size_t len;
    int tag;
    struct mrpc_conn_t* rconn;
};

struct mrpc_req_data_t {
    void* raw_data;
    size_t raw_data_len;
    struct mrpc_conn_t* rconn;
};

struct mrpc_conn_t {
    struct minirpc_t* rpc;
    int stage;
    size_t length;
    struct net_connection_t* conn;
    /* This 2 areas are embedded here in which it makes our code faster */
    struct mrpc_res_data_t response;
    struct mrpc_req_data_t request;
};

enum {
    RESPONSE_TAG_RSP,
    RESPONSE_TAG_LOG,
    RESPONSE_TAG_ERR,
    RESPONSE_TAG_DONE
};

int mrpc_get_package_size( void* buf , size_t sz , size_t* len )  {
    if( sz < 2 )
        return -1;

    if( decode_size(len,CAST(char*,buf)+1,sz-2) <0 )
        return -1;

    return 0;
}

static
void mrpc_request_parse_fail( struct minirpc_t* rpc , struct mrpc_conn_t* conn , int ec ) {
    conn->response.tag = RESPONSE_TAG_ERR;
    conn->response.buf = NULL;
    conn->response.len = 0;
    conn->response.rconn = conn;
    mq_enqueue(rpc->res_q,&(conn->response));
}

int mrpc_request_recv( struct minirpc_t* rpc , struct mrpc_request_t* req , void** conn ) {
    struct mrpc_req_data_t* data;
    int ret = mq_dequeue(rpc->req_q,CAST(void*,&data));
    int ec;
    if( ret != 0 ) {
        return -1;
    }
    *conn = data->rconn;
    ec = mrpc_request_parse(data->raw_data,data->raw_data_len,req);
    if( ec != 0 ) {
        mrpc_request_parse_fail( rpc, CAST(struct mrpc_conn_t*,*conn) , ec );
        return ec;
    }
    return 0;
}

void mrpc_response_send( struct minirpc_t* rpc ,
                          const struct mrpc_request_t* req ,
                          void* opaque , const struct val_t* result , int ec ) {
    struct mrpc_response_t response;
    struct mrpc_conn_t* conn = CAST(struct mrpc_conn_t*,opaque);

    assert(req->method_type != MRPC_NOTIFICATION);

    response.error_code = ec;
    strcpy(response.method_name,req->method_name);
    response.method_name_len = req->method_name_len;
    response.method_type = req->method_type;

    response.transaction_id[0] = req->transaction_id[0];
    response.transaction_id[1] = req->transaction_id[1];
    response.transaction_id[2] = req->transaction_id[2];
    response.transaction_id[3] = req->transaction_id[3];

    /* this copy is fine here since we don't really use this pointer */
    response.result = *result;

    /* serialization of the response objects */
    conn->response.buf = mrpc_response_serialize(&response,&conn->response.len);
    conn->response.rconn = conn;
    conn->response.tag = RESPONSE_TAG_RSP;

    /* send back the processor queue */
    mq_enqueue(rpc->res_q,&(conn->response));
}

void mrpc_response_done( struct minirpc_t* rpc , void* conn ) {
    struct mrpc_conn_t* rconn=CAST(struct mrpc_conn_t*,conn);
    rconn->response.tag = RESPONSE_TAG_DONE;
    rconn->response.rconn = rconn;
    mq_enqueue(rpc->res_q,&(rconn->response));
}

static
void do_log( struct minirpc_t* minirpc , const char* fmt , ... ) {
    va_list vlist;
    int ret;
    char buf[1024];
    va_start(vlist,fmt);
    ret=vsprintf(buf,fmt,vlist);
    assert( ret < 1024 );
    fwrite(buf,1,ret,minirpc->logf);
}

void mrpc_write_log( struct minirpc_t* rpc , const char* fmt , ... ) {
    int ret;
    char buf[1024];
    va_list vlist;
    struct mrpc_res_data_t* res;

    va_start(vlist,fmt);
    ret = vsprintf(buf,fmt,vlist);
    if( ret <=0 )
        return;

    res = malloc(sizeof(*res) +ret+1);
    VERIFY(res);
    res->buf = CAST(char*,res)+sizeof(*res);
    res->len = CAST(size_t,res+1);
    memcpy(res->buf,buf,ret+1);
    res->rconn = NULL;
    res->tag = RESPONSE_TAG_LOG;
    mq_enqueue(rpc->res_q,res);
}

/* This callback function will be used for each connection */
static
int mrpc_do_read( struct net_connection_t* conn , struct mrpc_conn_t* rconn ) {
    if( rconn->stage == PENDING_REPLY ) {
        rconn->stage = CONNECTION_FAILED;
        return NET_EV_IDLE;
    } else {
        /* Get the length bytes */
        if( rconn->length == 0 ) {
            size_t sz = net_buffer_readable_size(&(conn->in));
            void* data = net_buffer_peek(&(conn->in),&sz);
            if( mrpc_get_package_size(data,sz,&(rconn->length)) != 0 ) {
                return NET_EV_READ;
            }
        }
        /* If we reach here, we already get the package size */
        if( rconn->length == net_buffer_readable_size(&(conn->in)) ) {
            size_t sz = rconn->length;
            void* data = net_buffer_peek(&(conn->in),&sz);
            rconn->request.raw_data = data;
            rconn->request.raw_data_len = sz;
            rconn->request.rconn = rconn;
            rconn->stage = EXECUTE_RPC;
            mq_enqueue(rconn->rpc->req_q,&(rconn->request));
            return NET_EV_IDLE;
        } else {
            if( rconn->length < net_buffer_readable_size(&(conn->in)) ) {
                return NET_EV_CLOSE;
            } else {
                return NET_EV_READ;
            }
        }
    }
}

static
int mrpc_on_conn( int ev , int ec , struct net_connection_t* conn ) {
    struct mrpc_conn_t* rconn = CAST(struct mrpc_conn_t*,conn->user_data);
    if( ec != 0 ) {
        do_log(rconn->rpc,0,"network error:%d",ec);
        return NET_EV_CLOSE;
    } else {
        if( ev & NET_EV_EOF ) {
            if( rconn->stage == PENDING_REPLY ) {
                rconn->stage = CONNECTION_FAILED;
                return NET_EV_IDLE;
            } else {
                slab_free(&(rconn->rpc->conn_slab),rconn);
                return NET_EV_CLOSE;
            }
        } else if( ev & NET_EV_READ ) {
            return mrpc_do_read(conn,rconn);
        } else if( ev & NET_EV_WRITE ) {
            assert( rconn->stage == PENDING_REPLY );
            conn->timeout = MRPC_DEFAULT_TIMEOUT_CLOSE;
            slab_free(&(rconn->rpc->conn_slab),rconn);
            conn->user_data = NULL;
            return NET_EV_CLOSE | NET_EV_TIMEOUT;
        } else {
            assert(0);
            return NET_EV_CLOSE;
        }
    }
}

/*
 * This is the main poller function that serves as a idle function.
 * This function will be invoked as a timer fashion and it is used
 * to consume the data inside of the response queue.
 */

static
int mrpc_on_poll( int ev , int ec , struct net_connection_t* conn ) {
    int i = MRPC_DEFAULT_OUTBAND_SIZE;
    struct minirpc_t* rpc=CAST(struct minirpc_t*,conn->user_data);
    while( i!= 0 ) {
        void* data;
        int ret = mq_dequeue(rpc->res_q,&data);
        struct mrpc_res_data_t* res;
        if( ret != 0 )
            break;
        res = CAST(struct mrpc_res_data_t*,data);
        switch(res->tag) {
        case RESPONSE_TAG_RSP:
            if( res->rconn->stage == CONNECTION_FAILED ) {
                free(res->buf);
                net_stop(res->rconn->conn);
                slab_free(&(rpc->conn_slab),res->rconn);
                break;
            } else {
                res->rconn->stage = PENDING_REPLY;
                net_buffer_produce( &(res->rconn->conn->out), res->buf,  res->len);
                net_post( res->rconn->conn, NET_EV_WRITE);
                free(res->buf);
                res->buf = NULL;
                res->len = 0;
                break;
            }
        case RESPONSE_TAG_LOG:
            do_log( rpc , "%s" , CAST(const char*,res->buf) );
            free(res);
            break;
        case RESPONSE_TAG_ERR:
            res->rconn->conn->timeout = MRPC_DEFAULT_TIMEOUT_CLOSE;
            net_post(res->rconn->conn,NET_EV_CLOSE|NET_EV_TIMEOUT);
            slab_free(&(rpc->conn_slab),res->rconn);
            break;
        case RESPONSE_TAG_DONE:
            net_stop(res->rconn->conn);
            slab_free(&(rpc->conn_slab),res->rconn);
            break;
        default: assert(0); break;
        }
        --i;
    }
    conn->timeout = MRPC_DEFAULT_POLL_TIMEOUT;
    return NET_EV_TIMEOUT;
}

/* This is the main function for doing the accept operations */

static
int mrpc_on_accept( int ec , struct net_server_t* ser , struct net_connection_t* conn ) {
    if( ec == 0 ) {
        struct minirpc_t* rpc = CAST(struct minirpc_t*,ser->user_data);
        struct mrpc_conn_t* rconn = CAST(struct mrpc_conn_t*,slab_malloc(&(rpc->conn_slab)));

        conn->user_data = rconn;
        rconn->conn = conn;
        rconn->length = 0;
        rconn->rpc = rpc;
        rconn->stage = PENDING_REQUEST_OR_INDICATION;
        rconn->response.rconn = rconn;
        rconn->request.rconn = rconn;

        /* hook the callback function here */
        conn->cb = mrpc_on_conn;
        return NET_EV_READ;
    }
    return NET_EV_CLOSE;
}

void mrpc_init() {
    net_init();
}

#ifndef NDEBUG
static int MRPC_INSTANCE_NUM =0;
#endif

static
void
mrpc_clean( struct minirpc_t* rpc ) {
    mq_destroy(rpc->req_q);
    mq_destroy(rpc->res_q);
    slab_destroy(&(rpc->conn_slab));
    fclose(rpc->logf);
    net_server_destroy(&(rpc->server));
    free(rpc);
}

struct minirpc_t*
mrpc_create( const char* logf_name , const char* addr ) {
    struct minirpc_t* rpc = malloc(sizeof(*rpc));
    struct net_connection_t* conn;
    int ret;

    assert( MRPC_INSTANCE_NUM == 0 );
    VERIFY(rpc);

    /* initialize RPC object */
    slab_create(&(rpc->conn_slab),sizeof(struct mrpc_conn_t),MRPC_DEFAULT_RESERVE_MEMPOOL);
    rpc->req_q = mq_create();
    rpc->res_q = mq_create();
    rpc->logf = fopen(logf_name,"a+");
    if( rpc->logf == NULL ) {
        free(rpc);
        return NULL;
    }

    /* initialize the poller callback */
    ret = net_server_create(&(rpc->server),addr,mrpc_on_accept);
    if( ret != 0 ) {
        do_log(rpc,"cannot create server with address:%s",addr);
        fclose(rpc->logf);
        free(rpc);
        return NULL;
    }

    /* initialize poller callback */
    conn = net_timer(&(rpc->server),mrpc_on_poll,rpc,MRPC_DEFAULT_POLL_TIMEOUT);
    if( conn == NULL ) {
        do_log(rpc,"cannot create timeout event");
        mrpc_clean(rpc);
        return NULL;
    }

    /* initialize the user data */
    rpc->server.user_data = rpc;

#ifndef NDEBUG
    ++MRPC_INSTANCE_NUM;
#endif /* NDEBUG */

    return rpc;
}

int mrpc_run( struct minirpc_t * rpc ) {
    for( ;; ) {
        if( net_server_poll(&(rpc->server),-1,NULL) < 0 ) {
            do_log(rpc,"Network error:%s",strerror(errno));
            return -1;
        }
    }
}

int mrpc_poll( struct minirpc_t * rpc ) {
    int ret;
    if( (ret = net_server_poll(&(rpc->server),-1,NULL)) < 0 ) {
        do_log(rpc,"Network error:%s",strerror(errno));
        return -1;
    }
    return ret;
}

void mrpc_varchar_create( struct varchar_t* varchar , const char* str , int own ) {
    if( own ) {
        varchar->len = strlen(str);
        varchar->val=str;
    } else {
        varchar->len = strlen(str);
        if( varchar->len < MRPC_MAX_LOCAL_VAR_CHAR_LEN ) {
            strcpy(varchar->buf,str);
            varchar->val = varchar->buf;
        } else {
            varchar->val = malloc(varchar->len+1);
            VERIFY(varchar->val);
            memcpy( CAST(void*,varchar->val) ,str,varchar->len+1);
        }
    }
}

void mrpc_varchar_destroy( struct varchar_t* varchar ) {
    if(varchar->buf != varchar->val) {
        free(CAST(void*,varchar->val));
    }
}

/* client function */
static
void gen_transaction_id( char transaction_id[4] ) {
    srand(CAST(unsigned int,time(NULL)));
    transaction_id[0] = rand() % 255;
    srand(transaction_id[1]);
    transaction_id[1] = rand() % 255;
    srand(transaction_id[2]);
    transaction_id[2] = rand() % 255;
    srand(transaction_id[3]);
    transaction_id[3] = rand() % 255;
}

static
void mrpc_request_clean( struct mrpc_request_t* req ) {
    size_t i;

    for( i = 0 ; i < req->par_size ; ++i ) {
        if( req->par[i].type == MRPC_VARCHAR ) {
            mrpc_varchar_destroy(&(req->par[i].value.varchar));
        }
    }
}

static
void* mrpc_request_vserialize( size_t* len , int method_type ,const char* method_name , const char* par_fmt , va_list vl ) {
    struct mrpc_request_t req;
    void* seria_data = NULL;
    int i ;

    /* set request method type */
    req.method_type = method_type;

    /* generate transaction id here */
    gen_transaction_id(req.transaction_id);

    /* method name */
    assert( strlen(method_name) < MRPC_MAX_METHOD_NAME_LEN );
    req.method_name_len = strlen(method_name);
    strcpy(req.method_name,method_name);

    /* parameter list */
    req.par_size = 0;

    for( i = 0 ; par_fmt[i] ; ++i ) {
        if( par_fmt[i] == '%' ) {
            switch(par_fmt[i+1]) {
            case 'd':
                req.par[req.par_size].type = MRPC_INT;
                req.par[req.par_size].value.integer = va_arg(vl,int);
                break;
            case 'u':
                req.par[req.par_size].type = MRPC_UINT;
                req.par[req.par_size].value.uinteger= va_arg(vl,unsigned int);
                break;
            case 's':
                req.par[req.par_size].type = MRPC_VARCHAR;
                mrpc_varchar_create(&(req.par[req.par_size].value.varchar),va_arg(vl,const char*),0);
                break;
            default: goto fail;
            }
            ++i;
            ++req.par_size;
        }
    }
    return mrpc_request_msg_serialize(&req,len);
fail:
    mrpc_request_clean(&req);
    if(seria_data)
        free(seria_data);
    return NULL;
}

static
int mrpc_request_do_send( socket_t fd , const void* data, size_t sz ) {
    size_t offset = 0;
    do {
        int snd_sz = send(fd,CAST(const char*,data)+offset,sz,0);
        if( snd_sz < 0 ) {
            if( snd_sz == EAGAIN || snd_sz == EINTR )
                continue;
            else {
                return -1;
            }
        }
        offset += snd_sz;
    } while( offset < sz );
    assert( offset == sz );
    return 0;
}

#define TBUF_LENGTH (2+sizeof(size_t))

#define STACK_BUFF_SIZE (1024*10)

static
int mrpc_request_do_recv( socket_t fd , struct mrpc_response_t* resp ) {
    int ret;
    char sbuf[STACK_BUFF_SIZE]; /* 10 kb buffer on stack which doesn't need heap allocation */
    char* hbuf=NULL;            /* buffer on heap , once we found out that we cannot hold it */
    char* buf = sbuf;
    size_t buf_cap = STACK_BUFF_SIZE;
    size_t buf_sz = 0;
    size_t pkg_sz = 0;

    while(1) {
        ret = recv(fd,buf+buf_sz,buf_cap-buf_sz,0);
        if( ret <= 0 ) {
            if( ret == EINTR || ret == EAGAIN ) {
                continue;
            } else {
                ret = -1;
                break;
            }
        }

        if( mrpc_get_package_size(buf,ret+buf_sz,&pkg_sz) == 0 ) {
            if( pkg_sz > STACK_BUFF_SIZE ) {
                hbuf = malloc(pkg_sz);
                VERIFY(hbuf);
                memcpy(hbuf,sbuf,buf_sz+ret);
                buf=hbuf;
                buf_cap=pkg_sz;
            }
            buf_sz+=ret;
        } else {
            buf_sz += ret;
            assert(buf_sz < buf_cap);
            continue;
        }

        if( pkg_sz == buf_sz ) {
            ret = mrpc_response_parse(buf,buf_sz,resp );
            break;
        }
    }
    if( hbuf )
        free(hbuf);
    return ret;
}

int mrpc_request( const char* addr , int method_type , const char* method_name , struct mrpc_response_t* res , const char* par_fmt , ... ) {
    va_list vl;
    int ret = 0;
    void* seria_data = NULL;
    size_t seria_sz = 0;
    socket_t fd = invalid_socket_handler;

    assert( method_type == MRPC_FUNCTION || method_type == MRPC_NOTIFICATION );

    va_start(vl,par_fmt);
    seria_data = mrpc_request_vserialize(&seria_sz,method_type,method_name,par_fmt,vl);
    if( seria_data == NULL )
        return -1;

    /* connect to the peer side */
    fd = net_block_client_connect(addr);
    if( fd == invalid_socket_handler ) {
        ret = -1;
        goto done;
    }

    /* sending out the data now */
    if( mrpc_request_do_send(fd,seria_data,seria_sz) != 0 ) {
        ret = -1;
        goto done;
    }

    /* when we reach here, we have already sent out all the data
     * just blocking for waiting for the incoming traffic here */
    if( mrpc_request_do_recv(fd,res) != 0 ) {
        ret = -1;
        goto done;
    }

done:
    free(seria_data);
    if( fd != invalid_socket_handler )
        closesocket(fd);
    return ret;
}

void* mrpc_request_serialize( size_t* len , int method_type , const char* method_name , const char* par_fmt, ... ) {
    va_list vl;
    va_start(vl,par_fmt);
    return mrpc_request_vserialize(len,method_type,method_name,par_fmt,vl);
}

