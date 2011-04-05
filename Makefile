DFLAGS =
LFLAGS =
CFLAGS = -std=c99 -Wall -msse2 -D_GNU_SOURCE -fms-extensions -g -O2 $(DFLAGS)
MAKEDEPEND = gcc -M $(CPPFLAGS) -o $*.d $<

SRCS = testhashtable.c benchmarkhashtable.c \
	smphashtable.c onewaybuffer.c localmem.c \
	util.c alock.c ia32msr.c ia32perf.c

LIB_OBJECTS = smphashtable.o onewaybuffer.o localmem.o \
	util.o alock.o ia32msr.o ia32perf.o

all: testhashtable benchmarkhashtable 

testhashtable: testhashtable.o $(LIB_OBJECTS)
	gcc -o testhashtable testhashtable.o  $(LIB_OBJECTS) -lpthread -lm $(LFLAGS) 

benchmarkhashtable: benchmarkhashtable.o $(LIB_OBJECTS)
	gcc -o benchmarkhashtable benchmarkhashtable.o  $(LIB_OBJECTS) -lpthread -lm -lprofiler $(LFLAGS)

%.P : %.c
				$(MAKEDEPEND)
				@sed 's/\($*\)\.o[ :]*/\1.o $@ : /g' < $*.d > $@; \
					rm -f $*.d; [ -s $@ ] || rm -f $@

include $(SRCS:.c=.P)

.PHONY: clean
clean: 
	rm testhashtable benchmarkhashtable *.o 

