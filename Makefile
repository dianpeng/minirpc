all: mini-rpc
PRIVATE=private/coder.h private/coder.c private/conf.h private/mem.h private/mem.c private/mq.h private/mq.c private/network.c private/network.h


mini-rpc.o: $(PRIVATE) minirpc.h minirpc-service.h minirpc-service.c
	gcc -c minirpc.c -O3 -o mini-rpc.o
	
mini-rpc: mini-rpc.o
	ar rcs libmini-rpc.a mini-rpc.o
	
clean:
	rm -f *.o *a
