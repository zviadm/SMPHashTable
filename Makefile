CFLAGS := -std=c99 -Wall

lib_objects = smphashtable.o

all: testhashtable

testhashtable: testhashtable.o $(lib_objects)
	gcc -o testhashtable testhashtable.o  $(lib_objects) -lpthread # -pg -lprofiler

smphashtable.o: smphashtable.c smphashtable.h

.PHONY: clean
clean: 
	rm testhashtable *.o 

