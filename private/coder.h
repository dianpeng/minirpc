#ifndef CODER_H_
#define CODER_H_
#include <stddef.h>

/* variant */
int encode_int( int val ,char buf[5] );
int encode_uint( unsigned int val , char buf[5] );
int decode_int( int* val , const char* buf , size_t len );
int decode_uint( unsigned int* val , const char* buf , size_t len );
int encode_size( size_t val , char* buf , size_t buf_length );
int decode_size( size_t* val , const char* buf , size_t len );

/* this function helps to determine how much memory will be consumed for
 * encoding the variant type internally */

int encode_size_int( int val );
int encode_size_uint( unsigned int val );
int encode_size_size( size_t size );

/* fixed */
void encode_fushort( unsigned short val , char buf[2] );
void decode_fushort( unsigned short* val , const char* buf );
void encode_fuint( unsigned int val , char buf[4] );
void decode_fuint( unsigned int* val , const char* buf );

#define encode_byte(val,c) (*CAST(unsigned char*,c)=(val))
#define decode_byte(val,c) (*(val)=*CAST(unsigned char*,c))


#endif /* CODER_H_ */

