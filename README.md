# Mini RPC

## Feature
A small RPC implementation. 
1. Fast , although limited by system call select , every 512 request will be finished in about 80 milliseconds. Which
 is way more than enough for small network or embedded device.
2. Small , the library is just 200-300 KB. 
3. Self contained library , the library doesn't require any third party and is strict compatible with Linux/Windows and
 also Ansi C standard.
4. Simple, no rocket science here. Just absolute minimum and intuitive function , no IDL generation , only ANSI C 
    is needed here.
5. Easy to use, to set up a server, the user just needs to know 4-5 API then you could have a single IO thread with 
    multiple backend thread pool architecture ; for client user ,only 1 API is needed.
6. Efficient, wire protocol is entirely binary based, integer is encoded using Base128 , and string is encoded
    as slice. Overhead per packet is very small.
	
## Tutorial
 Issue a request on client:
	int mrpc_request( const char* addr, int method_type , const char* method_name ,
                  struct mrpc_response_t* res , const char* par_fmt , ... );
				  
 Set up a server print a hello world:
	See server.c for detail

## Platform 
1. Linux
2. Windows

## Licence
 MIT
 


