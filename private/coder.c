#include "coder.h"
#include "conf.h"

#include <limits.h>
#include <assert.h>

/* Unsigned int using base 128 */
static const unsigned int UINT_3BYTE = (1 <<(7*3)) -1;
static const unsigned int UINT_4BYTE = (1 <<(7*4)) -1;
static const unsigned int UINT_2BYTE = (1 <<(7*2)) -1;
static const unsigned int UINT_1BYTE = 127;


int encode_uint( unsigned int val ,char buf[5] ) {
    if( val > UINT_3BYTE ) {
        if( val > UINT_4BYTE ) {
            goto encode_5;
        } else {
            goto encode_4;
        }
    } else {
        if( val > UINT_2BYTE ) {
            goto encode_3;
        } else {
            if( val > UINT_1BYTE ) {
                goto encode_2;
            } else {
                goto encode_1;
            }
        }
    }

encode_1:
    buf[0] = val;
    return 1;
encode_2:
    buf[0] = (val & 0x000000ff) | (1<<7);
    buf[1] = ((val>>7) & 0x000000ff);
    return 2;
encode_3:
    buf[0] = (val & 0x000000ff) | (1<<7);
    buf[1] = ((val>>7) & 0x000000ff) | (1<<7);
    buf[2] = ((val>>14) & 0x000000ff);
    return 3;
encode_4:
    buf[0] = (val &0x000000ff) | (1<<7);
    buf[1] = ((val>>7) & 0x000000ff) | (1<<7);
    buf[2] = ((val>>14) & 0x000000ff) | (1<<7);
    buf[3] = ((val>>21) & 0x000000ff);
    return 4;
encode_5:
    buf[0] = (val &0x000000ff) | (1<<7);
    buf[1] = ((val>>7) & 0x000000ff) | (1<<7);
    buf[2] = ((val>>14) & 0x000000ff) | (1<<7);
    buf[3] = ((val>>21) & 0x000000ff) | (1<<7);
    buf[4] = ((val>>28) & 0x000000ff);
    return 5;
}

int decode_uint( unsigned int* val , const char* buf , size_t len ) {
    size_t plen = MIN(len,5);
    *val = buf[0] & 127;

#define RET(X) \
    do { \
        if( plen == 0 ) \
            return -1; \
        if( !(buf[0] & (1<<8)) ) \
            return X; \
        ++buf; \
        --plen; \
    } while(0)

    RET(1);
    *val += (buf[0] &127) << 7;
    RET(2);
    *val += (buf[0] &127) << 14;
    RET(3);
    *val += (buf[0] &127) << 21;
    RET(4);
    *val += (buf[0] &127) << 28;
    RET(5);
    return -1;

#undef RET
}

/* Signed integer is encoded by base 128 with zigzag ,
 * at most 5 bytes are used to encode a 4 byte number */
int encode_int( int val , char buf[5] ) {
    return encode_uint( (val<<1) ^ (val>>31) , buf );
}

int decode_int( int* val , const char* buf , size_t len ) {
    unsigned int zigzagval;
    int ret = decode_uint(&zigzagval,buf,len);
    if( ret <0 )
        return ret;
    *val = (zigzagval >> 1) ^ (-CAST(int,zigzagval & 1));
    return ret;
}

int encode_size( size_t val , char* buf , size_t buf_len ) {
    unsigned char* ubuf = CAST(unsigned char*,buf);
    if( val < 255 ) {
        ubuf[0]=CAST(unsigned char,val);
        return 1;
    } else {
        size_t i;
        ubuf[0]=255;
        for( i = 1 ; i < sizeof(size_t) + 1; ++i ) {
            ubuf[i]=CAST(unsigned char,val&0xff);
            val>>=8;
            if( i == buf_len )
                return -1;
        }
        return i;
    }
}

int decode_size( size_t* val , const char* buf , size_t len ) {
    unsigned char* ubuf = CAST(unsigned char*,buf);
    assert( len >= 1 && buf != NULL );
    if(*ubuf <255) {
        *val = *ubuf;
        return 1;
    } else {
        int i;
        *val = 0;

        for( i = 1 ; i < sizeof(size_t) + 1 ; ++i ) {
            if( i == len )
                return -1;

            *val += ( ubuf[i] << ((i-1)*8));
        }
        return i;
    }
}

int encode_size_int( int val ) {
    return encode_size_uint( (val<<1) ^ (val>>31) );
}

int encode_size_uint( unsigned int val ) {
    if( val > UINT_3BYTE ) {
        if( val > UINT_4BYTE ) {
            return 5;
        } else {
            return 4;
        }
    } else {
        if( val > UINT_2BYTE ) {
            return 3;
        } else {
            if( val > UINT_1BYTE ) {
                return 2;
            } else {
                return 1;
            }
        }
    }
}

int encode_size_size( size_t size ) {
    if( size < 255 )
        return 1;
    else
        return 1 + sizeof(size_t);
}

/* Fixed , just encode everything using big endian */
void encode_fint( int val , char buf[4] ) {
    buf[0] = val & 0x000000ff;
    buf[1] = (val>>8) & 0x000000ff;
    buf[2] = (val>>16) & 0x000000ff;
    buf[3] = (val>>24) & 0x000000ff;
}

void decode_fint( unsigned int* val , const char* buf ) {
    *val = buf[0] | (buf[1] << 8) | (buf[2] <<16) | (buf[3] << 24);
}

void encode_fshort( unsigned short val , char buf[2] ) {
    buf[0] = val & 0x000000ff;
    buf[1] = (val>>8) & 0x000000ff;
}

void decode_fshort( unsigned short* val , const char* buf ) {
    *val = buf[0] | (buf[1] <<8);
}
