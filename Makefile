CFLAGS := -std=c99 -Wall -msse2 -D_GNU_SOURCE

lib_objects = onewaybuffer.o smphashtable.o

all: testhashtable

testhashtable: testhashtable.o $(lib_objects)
	gcc -o testhashtable testhashtable.o  $(lib_objects) -lpthread # -pg -lprofiler

smphashtable.o: smphashtable.c smphashtable.h onewaybuffer.o
onewaybuffer.o: onewaybuffer.c onewaybuffer.h

.PHONY: clean
clean: 
	rm testhashtable *.o 

