#ifndef NETWORK_H_
#define NETWORK_H_
#include <stddef.h>

#ifdef _WIN32
#define FD_SETSIZE 1024
#include <WinSock2.h>
#include <windows.h>
typedef SOCKET socket_t;
typedef int socklen_t;
#define invalid_socket_handler INVALID_SOCKET
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
typedef int socket_t;
#define invalid_socket_handler -1
#define closesocket close
#endif // _WIN32

#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

enum {
    NET_EV_NULL  = 0,
    NET_EV_READ  = 1,
    NET_EV_WRITE = 1 << 1,
    NET_EV_LINGER = 1 << 2,
    NET_EV_LINGER_SILENT = 1 << 3,
    NET_EV_CLOSE = 1 << 4 ,
    NET_EV_REMOVE= 1 << 5 ,
    NET_EV_EOF   = 1 << 6 ,
    NET_EV_CONNECT = 1 << 7,
    NET_EV_TIMEOUT = 1 << 8,
    NET_EV_IDLE = 1 << 15,
    // error
    NET_EV_ERR_READ = 1<<16,
    NET_EV_ERR_WRITE= 1<<17,
    NET_EV_ERR_ACCEPT=1<<18,
    NET_EV_ERR_CONNECT = 1 << 19,

    NET_EV_NOT_LARGE_THAN = 1 << 20
};

struct net_buffer {
    void* mem;
    size_t consume_pos;
    size_t produce_pos;
    size_t capacity;
};

struct net_connection;

typedef int (*net_ccb_func)( int , int , struct net_connection* );

struct net_connection {
    struct net_connection* next;
    struct net_connection* prev;
    void* user_data;
    socket_t socket_fd;
    struct net_buffer in; // in buffer is the buffer for reading
    struct net_buffer out;// out buffer is the buffer for sending
    net_ccb_func cb;
    int pending_event;
    int timeout;
};

struct net_server;

typedef int (*net_acb_func)( int err_code , struct net_server* , struct net_connection* connection );

struct net_server {
    void* user_data;
    socket_t listen_fd;
    struct net_connection conns;
    socket_t ctrl_fd;
    net_acb_func cb;
    int last_io_time;
    void* reserve_buffer;
};

void net_init();

// server function
int net_server_create( struct net_server* , const char* addr , net_acb_func cb );
void net_server_destroy( struct net_server* );
int net_server_poll( struct net_server* ,int , int* );
int net_server_wakeup( struct net_server* );

// client function
socket_t net_block_client_connect( const char* addr );

// connect to a specific server
int net_non_block_client_connect( struct net_server* server ,
    const char* addr ,
    net_ccb_func cb ,
    void* udata ,
    int timeout );

int net_non_block_connect( struct net_connection* conn , const char* addr , int timeout );

struct net_connection* net_make_connection( struct net_server* server , net_ccb_func cb , 
    const char* addr , int timeout );

// timer and other socket function
struct net_connection* net_timer( struct net_server* server , net_ccb_func cb , void* udata , int timeout );
struct net_connection* net_fd( struct net_server* server , net_ccb_func cb , void* udata , socket_t fd , int pending_event );

// cancle another connection through struct net_connection_t* object , after this pointer is
// invalid, so do not store this pointer after calling this function
void net_stop( struct net_connection* conn );
void net_post( struct net_connection* conn , int ev );

// buffer function
void* net_buffer_consume( struct net_buffer* , size_t* );
void* net_buffer_peek( struct net_buffer*  , size_t* );
void net_buffer_produce( struct net_buffer* , const void* data , size_t );
struct net_buffer* net_buffer_create( size_t cap , struct net_buffer* );
void net_buffer_free( struct net_buffer* );
#define net_buffer_readable_size(b) ((b)->produce_pos - (b)->consume_pos)
#define net_buffer_writeable_size(b) ((b)->capacity - (b)->produce_pos)

#ifdef __cplusplus
}
#endif // __cplusplus

#endif // NETWORK_H_
