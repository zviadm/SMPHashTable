CFLAGS = -std=c99 -Wall -D_GNU_SOURCE -fms-extensions -g -O2 
MAKEDEPEND = gcc -M $(CFLAGS) -o $*.d $<

LIBSRC 		= smphashtable.c onewaybuffer.c localmem.c \
						hashclient.c util.c alock.c ia32msr.c ia32perf.c
SERVERSRC = hashserver.c hashserver2.c
TESTSRC 	= testhashtable.c testhashserver.c
BENCHSRC 	= benchhashtable.c benchhashserver.c 
SRCS = $(LIBSRC) $(SERVERSRC) $(TESTSRC) $(BENCHSRC) benchmemcached.c

LIBOBJS		 	= $(LIBSRC:.c=.o)
SERVERBINS 	= $(SERVERSRC:.c=)
TESTBINS 		= $(TESTSRC:.c=)
BENCHBINS 	= $(BENCHSRC:.c=)

BINS = $(SERVERBINS) $(TESTBINS) $(BENCHBINS) benchmemcached

all: $(BINS)

$(TESTBINS) $(SERVERBINS): %: %.o $(LIBOBJS)
	gcc -o $@ $^ -lpthread -lm

$(BENCHBINS): %: %.o $(LIBOBJS)
	gcc -o $@ $^ -lpthread -lm -lprofiler

benchmemcached: benchmemcached.o $(LIBOBJS)
	gcc -o benchmemcached benchmemcached.o $(LIBOBJS) -Wl,-Bstatic -lmemcached -Wl,-Bdynamic -lpthread -lm

%.P : %.c
				$(MAKEDEPEND)
				@sed 's/\($*\)\.o[ :]*/\1.o $@ : /g' < $*.d > $@; \
					rm -f $*.d; [ -s $@ ] || rm -f $@

include $(SRCS:.c=.P)

.PHONY: clean
clean: 
	rm -f $(BINS) *.o *.P

