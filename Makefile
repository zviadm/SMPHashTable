DFLAGS = 
CFLAGS = -std=c99 -Wall -msse2 -D_GNU_SOURCE -g -O2 $(DFLAGS)
					 
lib_objects = onewaybuffer.o smphashtable.o

all: testhashtable

nodebug:
	make clean; make DFLAGS=-DNDEBUG

testhashtable: testhashtable.o $(lib_objects)
	gcc -o testhashtable testhashtable.o  $(lib_objects) -lpthread # -pg -lprofiler

smphashtable.o: smphashtable.c smphashtable.h
onewaybuffer.o: onewaybuffer.c onewaybuffer.h
testhashtable.o: testhashtable.c

.PHONY: clean
clean: 
	rm testhashtable *.o 

