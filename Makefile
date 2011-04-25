DFLAGS =
LFLAGS =
CFLAGS = -std=c99 -Wall -D_GNU_SOURCE -fms-extensions -g -O2 $(DFLAGS)
MAKEDEPEND = gcc -M $(CPPFLAGS) -o $*.d $<

SRCS = testhashtable.c benchmarkhashtable.c \
			 testhashserver.c benchmarkhashserver.c \
			 hashserver.c hashclient.c \
			 smphashtable.c onewaybuffer.c localmem.c \
			 util.c alock.c ia32msr.c ia32perf.c

LIB_OBJECTS = hashclient.o smphashtable.o onewaybuffer.o localmem.o \
	util.o alock.o ia32msr.o ia32perf.o

all: testhashtable benchmarkhashtable testhashserver benchmarkhashserver hashserver benchmarkmemcached

testhashtable: testhashtable.o $(LIB_OBJECTS)
	gcc -o testhashtable testhashtable.o  $(LIB_OBJECTS) -lpthread -lm $(LFLAGS) 

benchmarkhashtable: benchmarkhashtable.o $(LIB_OBJECTS)
	gcc -o benchmarkhashtable benchmarkhashtable.o  $(LIB_OBJECTS) -lpthread -lm -lprofiler $(LFLAGS)

testhashserver: testhashserver.o $(LIB_OBJECTS)
	gcc -o testhashserver testhashserver.o  $(LIB_OBJECTS) -lpthread -lm $(LFLAGS) 

benchmarkhashserver: benchmarkhashserver.o $(LIB_OBJECTS)
	gcc -o benchmarkhashserver benchmarkhashserver.o  $(LIB_OBJECTS) -lpthread -lm $(LFLAGS)

hashserver: hashserver.o $(LIB_OBJECTS)
	gcc -o hashserver hashserver.o  $(LIB_OBJECTS) -lpthread -lm $(LFLAGS) 

benchmarkmemcached: benchmarkmemcached.o $(LIB_OBJECTS)
	gcc -o benchmarkmemcached benchmarkmemcached.o $(LIB_OBJECTS) /usr/local/lib/libmemcached.a -lpthread -lm $(LFLAGS)

%.P : %.c
				$(MAKEDEPEND)
				@sed 's/\($*\)\.o[ :]*/\1.o $@ : /g' < $*.d > $@; \
					rm -f $*.d; [ -s $@ ] || rm -f $@

include $(SRCS:.c=.P)

.PHONY: clean
clean: 
	rm -f testhashtable benchmarkhashtable testhashserver hashserver benchmarkmemcached *.o *.P

