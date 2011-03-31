CFLAGS = -std=c99 -Wall -msse2 -D_GNU_SOURCE -g -O2
					 
lib_objects = onewaybuffer.o smphashtable.o util.o alock.o ia32msr.o ia32perf.o localmem.o

all: testhashtable benchmarkhashtable

nodebug:
	make clean; make CFLAGS='-std=c99 -Wall -msse2 -D_GNU_SOURCE -g -O2 -DNDEBUG'

testhashtable: testhashtable.o $(lib_objects)
	gcc -o testhashtable testhashtable.o  $(lib_objects) -lpthread # -pg -lprofiler

benchmarkhashtable: benchmarkhashtable.o $(lib_objects)
	gcc -o benchmarkhashtable benchmarkhashtable.o  $(lib_objects) -lpthread # -pg -lprofiler

smphashtable.o: smphashtable.c smphashtable.h onewaybuffer.h localmem.h util.h alock.h
onewaybuffer.o: onewaybuffer.c onewaybuffer.h util.h
localmem.o: localmem.c localmem.h util.h
alock.o: alock.c alock.h util.h
util.o: util.c util.h
ia32perf.o: ia32perf.c ia32perf.h ia32msr.h
ia32msr.o: ia32msr.c ia32msr.h
testhashtable.o: testhashtable.c smphashtable.h util.h
benchmarkhashtable.o: benchmarkhashtable.c smphashtable.h util.h ia32perf.h

.PHONY: clean
clean: 
	rm testhashtable benchmarkhashtable *.o 

