#include "network.h"
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#ifndef _WIN32
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/time.h>
#endif // _WIN32


#ifdef __cplusplus
extern "C" {
#endif // __cplusplus

#define SERVER_CONTROL_DATA_LENGTH 128

#define MAXIMUM_IPV4_PACKET_SIZE 65536

#ifndef NDEBUG
#define VERIFY assert
#else
#define VERIFY(cond) do { \
    if(!cond) { \
    fprintf(stderr,"die:" #cond); abort(); }} while(0)
#endif // NDEBUG

#ifndef MULTI_SERVER_ENABLE
static char single_server_internal_buffer[MAXIMUM_IPV4_PACKET_SIZE];
#endif

#define cast(x,p) ((x)(p))

#ifndef min
#define min(x,y) ((x) < (y) ? (x) : (y))
#endif // min

// Internal message for linger options
enum {
    NET_EV_TIMEOUT_AND_CLOSE = 1 << 10
};

static void* mem_alloc( size_t cap ) {
    void* ret = malloc(cap);
    VERIFY(ret);
    return ret;
}

static void mem_free( void* ptr ) {
    assert(ptr);
    free(ptr);
}

static void* mem_realloc( void* ptr , size_t cap ) {
    void* ret;
    assert(cap !=0);
    ret = realloc(ptr,cap);
    VERIFY(ret);
    return ret;
}

static int str_to_sockaddr( const char* str , struct sockaddr_in* addr ) {
    int c1,c2,c3,c4,port;
    int ret = sscanf(str,"%u.%u.%u.%u:%u",&c1,&c2,&c3,&c4,&port);
    if( ret != 5 )  return -1;
    memset(addr,0,sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    addr->sin_addr.s_addr = htonl((c1<<24)+(c2<<16)+(c3<<8)+c4);
    return 0;
}

static void exec_socket( socket_t sock ) {
    assert(sock);
#ifdef _WIN32
    SetHandleInformation((HANDLE) sock, HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(sock, F_SETFD, FD_CLOEXEC);
#endif
}

static void nb_socket( socket_t sock ) {
#ifdef _WIN32
    unsigned long on = 1;
    ioctlsocket(sock, FIONBIO, &on);
#else
    int f = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, f | O_NONBLOCK);
#endif // _WIN32
}

static void reuse_socket( socket_t sock ) {
    int on = 1;
#ifdef _WIN32
    setsockopt(sock,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,cast(const char*,&on),sizeof(int));
#endif // _WIN32
    setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,cast(const char*,&on),sizeof(int));
}

// platform error
static int net_has_error() {
#ifdef _WIN32
    int ret = WSAGetLastError();
    if( ret == 0 ) return 0;
    else {
        if( ret == WSAEWOULDBLOCK || ret == WSAEINTR )
            return 0;
        else
            return ret;
    }
#else
    if( errno == 0 ) return 0;
    else if( errno != EAGAIN &&
        errno != EWOULDBLOCK &&
        errno != EINTR &&
        errno != EINPROGRESS &&
        errno != 0 ) return errno;
    else return 0;
#endif
}

static int get_time_millisec() {
#ifndef _WIN32
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return (int)(tv.tv_sec*1000 + (tv.tv_usec/1000));
#else
    static const uint64_t EPOCH = ((uint64_t) 116444736000000000ULL);
    SYSTEMTIME  system_time;
    FILETIME    file_time;
    uint64_t    time;
    GetSystemTime( &system_time );
    SystemTimeToFileTime( &system_time, &file_time );
    time =  ((uint64_t)file_time.dwLowDateTime )      ;
    time += ((uint64_t)file_time.dwHighDateTime) << 32;
    return (int)(system_time.wMilliseconds + (time - EPOCH) / 10000L);
#endif
}

// buffer internal data structure
// [--- write buffer -- -- extra ---]
//       read_pos
//                   write_pos
//                                  capacity

struct net_buffer_t* net_buffer_create( size_t cap , struct net_buffer_t* buf ) {
    if( cap == 0 )
        buf->mem = NULL;
    else
        buf->mem = mem_alloc(cap);
    buf->consume_pos = buf->produce_pos = 0;
    buf->capacity = cap;
    return buf;
}

void net_buffer_free( struct net_buffer_t* buf ) {
    if(buf->mem)
        mem_free(buf->mem);
    buf->consume_pos = buf->produce_pos = buf->capacity = 0;
}

void* net_buffer_consume( struct net_buffer_t* buf , size_t* size ) {
    int consume_size;
    void* ret;
    if( buf->mem == NULL ) { *size = 0 ; return NULL; }
    else {
        consume_size = min(*size,net_buffer_readable_size(buf));
        if( consume_size == 0 ) { *size = 0 ; return NULL; }
        ret = cast(char*,buf->mem) + buf->consume_pos;
        // advance the internal read pointer
        buf->consume_pos += consume_size;
        // checking if we can rewind or not
        if( buf->consume_pos == buf->produce_pos ) {
            buf->consume_pos = buf->produce_pos = 0;
        }
        *size = consume_size;
        return ret;
    }
}

void* net_buffer_peek( struct net_buffer_t*  buf , size_t* size ) {
    int consume_size;
    void* ret;
    if( buf->mem == NULL ) { *size = 0 ; return NULL; }
    else {
        consume_size = min(*size,net_buffer_readable_size(buf));
        if( consume_size == 0 ) { *size = 0 ; return NULL; }
        ret = cast(char*,buf->mem) + buf->consume_pos;
        *size = consume_size;
        return ret;
    }
}

void net_buffer_produce( struct net_buffer_t* buf , const void* data , size_t size ) {
    if( buf->capacity < size + buf->produce_pos ) {
        // We need to expand the memory
        size_t cap = size + buf->produce_pos;
        buf->mem = mem_realloc(buf->mem,cap);
        buf->capacity = cap;
    }
    // Write the data to the buffer position
    memcpy(cast(char*,buf->mem) + buf->produce_pos , data , size);
    buf->produce_pos += size;
}

static void* net_buffer_consume_peek( struct net_buffer_t* buf ) {
    if( buf->mem == NULL )
        return NULL;
    else {
        if( buf->consume_pos == buf->produce_pos )
            return NULL;
        else
            return cast(char*,buf->mem) + buf->consume_pos;
    }
}

static void net_buffer_consume_advance( struct net_buffer_t* buf , size_t size ) {
    if( buf->mem == NULL || buf->produce_pos < buf->consume_pos + size )
        return;
    buf->consume_pos += size;
    if(buf->consume_pos == buf->produce_pos) {
        buf->consume_pos = buf->produce_pos = 0;
    }
}

#define net_buffer_clear(buf) \
    do { \
        (buf)->capacity=(buf)->produce_pos=(buf)->consume_pos=0; \
        (buf)->mem = NULL; \
    } while(0)

// connection
static void connection_cb( int ev , int ec , struct net_connection_t* conn ) {
    if( conn->cb != NULL ) {
        conn->pending_event = conn->cb(ev,ec,conn);
    }
}

static struct net_connection_t* connection_create( socket_t fd ) {
    struct net_connection_t* conn = mem_alloc(sizeof(struct net_connection_t));
    conn->socket_fd = fd;
    net_buffer_clear(&(conn->in));
    net_buffer_clear(&(conn->out));
    conn->cb = NULL;
    conn->user_data = NULL;
    conn->timeout = -1;
    conn->pending_event = NET_EV_NULL;
    return conn;
}

// we always add the connection to the end of the list since this will
// make the newly added socket being inserted into the poll fdset quicker
#define connection_add(server,conn) \
    do { \
        conn->prev = server->conns.prev; \
        server->conns.prev->next = conn; \
        server->conns.prev = conn; \
        conn->next = &((server)->conns); \
    }while(0)

static struct net_connection_t* connection_destroy( struct net_connection_t* conn ) {
    struct net_connection_t* ret = conn->prev;
    // closing the underlying socket and this must be called at once
    conn->prev->next = conn->next;
    conn->next->prev = conn->prev;
    net_buffer_free(&(conn->in));
    net_buffer_free(&(conn->out));
    mem_free(conn);
    return ret;
}

static struct net_connection_t* connection_close( struct net_connection_t* conn ) {
    socket_t fd = conn->socket_fd;
    struct net_connection_t* ret = connection_destroy(conn);
    if( fd != invalid_socket_handler )
        closesocket(fd);
    return ret;
}

// server
int net_server_create( struct net_server_t* server, const char* addr , net_acb_func cb ) {
    struct sockaddr_in ipv4;
    server->conns.next = &(server->conns);
    server->conns.prev = &(server->conns);
    server->cb = cb;
    server->user_data = NULL;
    server->last_io_time = 0;
    if( addr != NULL ) {
        if( str_to_sockaddr(addr,&ipv4) != 0 )
            return -1;
        // socket stream
        server->listen_fd = socket(AF_INET,SOCK_STREAM,0);
        if( server->listen_fd == invalid_socket_handler )
            return -1;
        nb_socket(server->listen_fd);
        exec_socket(server->listen_fd);
        // reuse the addr
        reuse_socket(server->listen_fd);
        // bind
        if( bind(server->listen_fd,cast(struct sockaddr*,&ipv4),sizeof(ipv4)) != 0 ) {
            closesocket(server->listen_fd);
            server->listen_fd = invalid_socket_handler;
            return -1;
        }
        // listen
        if( listen(server->listen_fd,SOMAXCONN) != 0 ) {
            closesocket(server->listen_fd);
            return -1;
        }
    } else {
        // We don't have a dedicated listen server here
        server->cb = NULL;
        server->ctrl_fd = server->listen_fd = invalid_socket_handler;
    }

    // control socket
    server->ctrl_fd = socket(AF_INET,SOCK_DGRAM,0);
    if( server->ctrl_fd == invalid_socket_handler ) {
        if( server->listen_fd != invalid_socket_handler )
            closesocket(server->listen_fd);
        return -1;
    }
    nb_socket(server->ctrl_fd);
    exec_socket(server->ctrl_fd);
    memset(&ipv4,0,sizeof(ipv4));
    // setting the localhost address for the ctrl udp
    ipv4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ipv4.sin_family = AF_INET;
    ipv4.sin_port = htons(0);
    if( bind(server->ctrl_fd,cast(struct sockaddr*,&ipv4),sizeof(ipv4)) != 0 ) {
        if( server->listen_fd != invalid_socket_handler )
            closesocket(server->listen_fd);
        closesocket(server->ctrl_fd);
        server->listen_fd = invalid_socket_handler;
        server->ctrl_fd = invalid_socket_handler;
        return -1;
    }
#ifndef MULTI_SERVER_ENABLE
    server->reserve_buffer = single_server_internal_buffer;
#else
    server->reserve_buffer = mem_alloc(MAXIMUM_IPV4_PACKET_SIZE);
#endif // MULTI_SERVER_ENABLE
    return 0;
}

static void server_close_all_conns( struct net_server_t* server ) {
    struct net_connection_t* next = server->conns.next;
    struct net_connection_t* temp = NULL;
    while( next != &(server->conns) ) {
        temp = next->next;
        connection_close(temp);
        next = temp;
    }
}

void net_server_destroy( struct net_server_t* server ) {
    server_close_all_conns(server);
    if( server->ctrl_fd != invalid_socket_handler )
        closesocket(server->ctrl_fd);
    if( server->listen_fd != invalid_socket_handler )
        closesocket(server->listen_fd);
    server->conns.next = &(server->conns);
    server->conns.prev = &(server->conns);
    server->ctrl_fd = server->listen_fd = invalid_socket_handler;
#ifdef MULTI_SERVER_ENABLE
    if( server->reserve_buffer != NULL )
        mem_free(server->reserve_buffer);
#endif // MULTI_SERVER_ENABLE
}

int net_server_wakeup( struct net_server_t* server ) {
    char buffer[SERVER_CONTROL_DATA_LENGTH];
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    assert(server->ctrl_fd != invalid_socket_handler &&  server->listen_fd != invalid_socket_handler );
    memset(&addr,0,sizeof(addr));
    if( getsockname(server->ctrl_fd,cast(struct sockaddr*,&addr),&len) !=0 )
        return -1;
    return sendto(server->ctrl_fd,buffer,
        SERVER_CONTROL_DATA_LENGTH,0,cast(struct sockaddr*,&addr),len) >0 ? 1 : 0;
}

static void do_accept( struct net_server_t* server );
static void do_control( struct net_server_t* server );
static int do_write( struct net_connection_t* conn , int* error_code );
static int do_read( struct net_server_t* server , int* error_code , struct net_connection_t* conn );
static int do_connected( struct net_connection_t* conn , int* error_code );

#define ADD_FSET(fs,fd,mfd) \
    do { \
        FD_SET(fd,fs); \
        if( *(mfd) < fd ) { *(mfd) = fd; } \
    }while(0)

static int prepare_linger( struct net_connection_t* conn , fd_set* write , socket_t* max_fd ) {
    if( net_buffer_readable_size(&(conn->out)) ) {
        FD_SET(conn->socket_fd,write);
        if( *max_fd < conn->socket_fd )
            *max_fd = conn->socket_fd;
        return 0;
    }
    return -1;
}

static void prepare_fd( struct net_server_t* server , fd_set* read_set , fd_set* write_set , int* millis , socket_t* max_fd ) {
    struct net_connection_t* conn;
    // adding the whole connection that we already have to the sets
    for( conn = server->conns.next ; conn != &(server->conns) ; conn = conn->next ) {
        if( conn->pending_event & NET_EV_IDLE )
            continue;
        // timeout is a always configurable event
        if( (conn->pending_event & NET_EV_TIMEOUT) ||
            (conn->pending_event & NET_EV_TIMEOUT_AND_CLOSE) ) {
            if( conn->timeout >= 0 ) {
                if( (*millis >=0 && *millis > conn->timeout) || *millis < 0 ) {
                    *millis = conn->timeout;
                }
            }
        }
        // read/write , connect , lingerXXX , close
        if( (conn->pending_event & NET_EV_READ) || (conn->pending_event & NET_EV_WRITE) ) {
            assert( !(conn->pending_event & NET_EV_LINGER) &&
                !(conn->pending_event & NET_EV_LINGER_SILENT) &&
                !(conn->pending_event & NET_EV_CONNECT) &&
                !(conn->pending_event & NET_EV_CLOSE) );
            if( conn->pending_event & NET_EV_READ ) {
                ADD_FSET(read_set,conn->socket_fd,max_fd);
            }
            if( conn->pending_event & NET_EV_WRITE ) {
                ADD_FSET(write_set,conn->socket_fd,max_fd);
            }
        } else {
            if( (conn->pending_event & NET_EV_LINGER) || (conn->pending_event & NET_EV_LINGER_SILENT) ) {
                assert( !(conn->pending_event & NET_EV_CONNECT) &&
                    !(conn->pending_event & NET_EV_CLOSE) );
                if( prepare_linger(conn,write_set,max_fd) !=0 ) {
                    if( conn->pending_event & NET_EV_LINGER ) {
                        connection_cb(NET_EV_LINGER,0,conn);
                    }
                    if( conn->pending_event & NET_EV_TIMEOUT && conn->timeout > 0 )
                        conn->pending_event = NET_EV_TIMEOUT_AND_CLOSE;
                    else
                        conn->pending_event = NET_EV_CLOSE;
                }
            } else if( conn->pending_event & NET_EV_CONNECT ) {
                assert( !(conn->pending_event & NET_EV_CLOSE) );
                ADD_FSET(write_set,conn->socket_fd,max_fd);
            } else {
                // We just need to convert a NET_EV_CLOSE|NET_EV_TIMEOUT to
                // internal NET_EV_TIMEOUT_AND_CLOSE operations
                if( conn->pending_event & NET_EV_CLOSE && 
                    conn->pending_event & NET_EV_TIMEOUT && 
                    conn->timeout >0 ) {
                    conn->pending_event = NET_EV_TIMEOUT_AND_CLOSE;
                }
            }
        }
    }
}

static int dispatch( struct net_server_t* server , fd_set* read_set , fd_set* write_set , int time_diff ) {
    struct net_connection_t* conn;
    int ev , rw , ret ,ec;
    // 1. checking if we have control operation or not
    if( FD_ISSET(server->ctrl_fd,read_set) ) {
        do_control(server);
        return 1;
    }
    // 2. checking the accept operation is done or not
    if( server->listen_fd != invalid_socket_handler && FD_ISSET(server->listen_fd,read_set) ) {
        do_accept(server);
    }
    // 3. looping through all the received events in the list
    for( conn = server->conns.next ; conn != &(server->conns) ; conn = conn->next ) {
        if( conn->pending_event & NET_EV_IDLE )
            continue;
        ev = 0; ec = 0;
        // timeout
        if( (conn->pending_event & NET_EV_TIMEOUT) ||
            (conn->pending_event & NET_EV_TIMEOUT_AND_CLOSE) ) {
            if( conn->timeout <= time_diff ) {
                ev |= (conn->pending_event & NET_EV_TIMEOUT) ? NET_EV_TIMEOUT : NET_EV_TIMEOUT_AND_CLOSE;
            } else {
                conn->timeout -= time_diff;
            }
        }
        // connect
        if( (conn->pending_event & NET_EV_CONNECT) && FD_ISSET(conn->socket_fd,write_set) ) {
            // connection operation done, notify our user
            if( do_connected(conn,&ec) == 0 ) {
                ev |= NET_EV_CONNECT;
                connection_cb(ev,0,conn);
            } else {
                ev |= NET_EV_ERR_CONNECT;
                connection_cb(ev,ec,conn);
            }
            continue;
        }
        // read/write
        if( (conn->pending_event & NET_EV_WRITE) || (conn->pending_event & NET_EV_READ) ) {
            rw = 0; ec = 0;
            // checking read
            if( (conn->pending_event & NET_EV_READ) && FD_ISSET(conn->socket_fd,read_set) ) {
                ret = do_read(server,&ec,conn);
                if( ret == 0 ) {
                    ev |= NET_EV_EOF;
                } else if( ret < 0 ) {
                    ev |= NET_EV_ERR_READ;
                } else {
                    ev |= NET_EV_READ;
                }
                ++rw;
            }
            // checking write
            if( !(ev & NET_EV_ERR_READ) && (conn->pending_event & NET_EV_WRITE) && FD_ISSET(conn->socket_fd,write_set) ) {
                ret = do_write(conn,&ec);
                if( ret < 0 ) {
                    ev |= NET_EV_ERR_WRITE;
                } else {
                    ev |= NET_EV_WRITE;
                }
                ++rw;
            }
            // call the connection callback function here
            if( rw != 0 ) connection_cb(ev,ec,conn);
            continue;
        }
        // linger
        if( ((conn->pending_event & NET_EV_LINGER) || (conn->pending_event & NET_EV_LINGER_SILENT)) && FD_ISSET(conn->socket_fd,write_set) ) {
            ec = 0;
            ret = do_write(conn,&ec);
            if( ret <= 0 ) {
                conn->pending_event = NET_EV_CLOSE;
            } else if( net_buffer_readable_size(&(conn->out)) == 0 ) {
                if( conn->pending_event & NET_EV_LINGER ) {
                    connection_cb(NET_EV_LINGER,ec,conn);
                }
                if( (conn->pending_event & NET_EV_TIMEOUT) && (conn->timeout >0) ) {
                    conn->pending_event = NET_EV_TIMEOUT_AND_CLOSE;
                } else {
                    conn->pending_event = NET_EV_CLOSE;
                }
            }
            continue;
        }
        // if we reach here means only timeout is specified
        if( (conn->pending_event & NET_EV_TIMEOUT) && (ev & NET_EV_TIMEOUT) ) {
            connection_cb(NET_EV_TIMEOUT,0,conn);
        } else if( (conn->pending_event & NET_EV_TIMEOUT_AND_CLOSE) && (ev & NET_EV_TIMEOUT_AND_CLOSE) ) {
            // need to close this socket here
            conn->pending_event = NET_EV_CLOSE;
        }
    }
    return 0;
}

static void reclaim_socket( struct net_server_t* server ) {
    struct net_connection_t* conn;
    // reclaim all the socket that has marked it as CLOSE operation
    for( conn = server->conns.next ; conn != &(server->conns) ; conn = conn->next ) {
        if( conn->pending_event & NET_EV_CLOSE ) {
            conn = connection_close(conn);
        } else if( conn->pending_event & NET_EV_REMOVE ) {
            conn = connection_destroy(conn);
        }
    }
}

int net_server_poll( struct net_server_t* server , int millis , int* wakeup ) {
    fd_set read_set , write_set;
    socket_t max_fd = invalid_socket_handler;
    int active_num , return_num;
    struct timeval tv;
    int time_diff;
    int cur_time;

    FD_ZERO(&read_set);
    FD_ZERO(&write_set);

    // adding the listen_fd and ctrl_fd
    if( server->listen_fd != invalid_socket_handler )
        ADD_FSET(&read_set,server->listen_fd,&max_fd);
    ADD_FSET(&read_set,server->ctrl_fd,&max_fd);

    prepare_fd(server,&read_set,&write_set,&millis,&max_fd);

    // setting the timer
    if( millis >= 0 ) {
        tv.tv_sec = millis / 1000;
        tv.tv_usec = (millis % 1000) * 1000;
    }

    if( server->last_io_time == 0 )
        server->last_io_time = get_time_millisec();
    // start our polling mechanism
    if( max_fd == invalid_socket_handler )
        max_fd = 0;
    active_num = select(max_fd+1,&read_set,&write_set,NULL,millis >= 0 ? &tv : NULL);
    if( active_num < 0 ) {
        int err = net_has_error();
        if( err == 0 )
          return 0;
        else
          return -1;
    }
    return_num = active_num;
    cur_time = get_time_millisec();
    time_diff = cur_time - server->last_io_time;
    if( millis < 0 ) {
        server->last_io_time = cur_time;
    } else {
        if( time_diff > 0 )
            server->last_io_time = cur_time;
    }
    // if we have errno set to EWOULDBLOCK EINTER which typically
    // require us to re-enter the loop, we don't need to do this
    // what we need to do is just put this poll into the loop , so
    // no need to worry about the problem returned by the select

    if( active_num >= 0 ) {
        int w;
        if( time_diff == 0 )
            time_diff = 1;
        w = dispatch(server,&read_set,&write_set,time_diff);
        if( wakeup != NULL )
            *wakeup = w;
    }
    // 4. reclaim all the socket that has marked it as CLOSE operation
    reclaim_socket(server);
    return return_num;
}

#undef ADD_FSET

static void do_accept( struct net_server_t* server ) {
    struct net_connection_t* conn;
    int error_code;
    do {
        socket_t sock = accept(server->listen_fd,NULL,NULL);
        if( sock == invalid_socket_handler ) {
            error_code = net_has_error();
            if( error_code != 0 ) {
                server->cb(error_code,server,NULL);
            }
            return;
        } else {
            int pending_ev;
            nb_socket(sock);
            conn = connection_create(sock);
            connection_add(server,conn);
            conn->pending_event = NET_EV_CLOSE;
            pending_ev = server->cb(0,server,conn);
            if( conn->cb == NULL )
                conn->pending_event = NET_EV_CLOSE;
            else
                conn->pending_event = pending_ev;
        }
    } while(1);
}

static int do_read( struct net_server_t* server , int* error_code , struct net_connection_t* conn ) {
    int rd = recv( conn->socket_fd , server->reserve_buffer , MAXIMUM_IPV4_PACKET_SIZE , 0 );
    if( rd <= 0 ) {
        *error_code = net_has_error();
        return rd;
    } else {
        net_buffer_produce( &(conn->in) , server->reserve_buffer , rd );
        return rd;
    }
}

static int do_write( struct net_connection_t* conn , int* error_code ) {
    void* out = net_buffer_consume_peek(&(conn->out));
    int snd;
    if( out == NULL ) return 0;
    snd = send(conn->socket_fd,out,net_buffer_readable_size(&(conn->out)),0);
    if( snd <= 0 ) {
        *error_code = net_has_error();
        return snd;
    } else {
        net_buffer_consume_advance(&(conn->out),snd);
        return snd;
    }
}

static void do_control( struct net_server_t* server ) {
    char buffer[SERVER_CONTROL_DATA_LENGTH];
    recvfrom(server->ctrl_fd,buffer,SERVER_CONTROL_DATA_LENGTH,0,NULL,NULL);
}

static int do_connected( struct net_connection_t* conn , int* error_code ) {
    int val;
    socklen_t len = sizeof(int);
    // before we do anything we need to check whether we have connected to the socket or not
    getsockopt(conn->socket_fd,SOL_SOCKET,SO_ERROR,cast(char*,&val),&len);
    if( val != 0 ) {
        *error_code = val;
        return -1;
    } else {
        return 0;
    }
}

// client function
socket_t net_block_client_connect( const char* addr ) {
    struct sockaddr_in ipv4;
    int ret;
    socket_t sock;
    if( str_to_sockaddr(addr,&ipv4) != 0 ) {
        return invalid_socket_handler;
    } else {
        sock = socket(AF_INET,SOCK_STREAM,0);
        if( sock == invalid_socket_handler )
            return sock;
        reuse_socket(sock);
        ret = connect(sock,cast(struct sockaddr*,&ipv4),sizeof(ipv4));
        if( ret != 0 ) {
            closesocket(sock);
            return invalid_socket_handler;
        }
        return sock;
    }
}

int net_non_block_client_connect(struct net_server_t* server ,
    const char* addr ,
    net_ccb_func cb ,
    void* udata ,
    int timeout ) {
        struct net_connection_t* conn = connection_create(invalid_socket_handler);
        connection_add(server,conn);
        conn->cb = cb;
        conn->user_data = udata;
        if( net_non_block_connect(conn,addr,timeout) == NET_EV_REMOVE ) {
            if( conn->socket_fd == invalid_socket_handler ) {
                // error
                connection_close(conn);
                return -1;
            }
        }
        return 0;
}

int net_non_block_connect( struct net_connection_t* conn , const char* addr , int timeout ) {
    int ret;
    struct sockaddr_in ipv4;
    socket_t fd;
    if( str_to_sockaddr(addr,&ipv4) != 0 )
        return NET_EV_REMOVE;
    fd = socket(AF_INET,SOCK_STREAM,0);
    if( fd == invalid_socket_handler ) {
        return NET_EV_REMOVE;
    }
    nb_socket(fd);
    exec_socket(fd);
    reuse_socket(fd);
    ret = connect( fd , cast(struct sockaddr*,&ipv4) , sizeof(ipv4));
    if( ret != 0 && net_has_error() != 0 )  {
        closesocket(fd);
        return NET_EV_REMOVE;
    }
    conn->socket_fd = fd;
    conn->pending_event = NET_EV_CONNECT;
    if( ret != 0 && timeout >= 0 ) {
        conn->pending_event |= NET_EV_TIMEOUT;
        conn->timeout=  timeout;
    } else if( ret == 0 ) {
        connection_cb(NET_EV_CONNECT,0,conn);
        return conn->pending_event;
    }
    return conn->pending_event;
}

// timer and socket
struct net_connection_t* net_timer( struct net_server_t* server , net_ccb_func cb , void* udata , int timeout ) {
    struct net_connection_t* conn = connection_create(invalid_socket_handler);
    connection_add(server,conn);
    conn->cb = cb;
    conn->user_data = udata;
    conn->timeout = timeout;
    conn->pending_event = NET_EV_TIMEOUT;
    return conn;
}

struct net_connection_t* net_fd( struct net_server_t* server, net_ccb_func cb , void* data ,  socket_t fd , int pending_event ) {
    struct net_connection_t* conn = connection_create(fd);
    nb_socket(fd);
    exec_socket(fd);
    conn->cb = cb;
    conn->user_data = data;
    conn->pending_event = pending_event;
    return conn;
}

void net_stop( struct net_connection_t* conn ) {
    conn->pending_event = NET_EV_CLOSE;
}

void net_post( struct net_connection_t* conn , int ev ) {
    conn->pending_event = ev;
}

// platform problem
void net_init() {
#ifdef _WIN32
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
#endif // _WIN32
}

#ifdef __cplusplus
}
#endif // __cplusplus
