NAME := libminirpc
SOURCE := $(wildcard *.c) $(wildcard private/*.c)
OBJ := ${SOURCE:.c=.o}
LIB := *.a
CC=gcc
OFLAGS=-Os

.PHONY:all clean

all: $(NAME)

minirpc: $(OBJ)
	$(CC) -c $(SOURCE) $(FLASGS)
	
$(NAME): minirpc
	ar rcs libminirpc.a $(OBJ)

clean:
	rm -f *.o *.a private/*.o
