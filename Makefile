DFLAGS = -DAMD64
CFLAGS = -std=c99 -Wall -D_GNU_SOURCE -fms-extensions -g -O2 $(DFLAGS)
LFLAGS = -lpthread -lm -lrt
MAKEDEPEND = gcc -M $(CFLAGS) -o $*.d $<

LIBSRC = smphashtable.c onewaybuffer.c localmem.c \
				 util.c alock.c ia32msr.c ia32perf.c
SRCS = $(LIBSRC) \
			 testhashtable.c \
			 benchhashtable.c \
			 hashserver2.c \
			 hashclient.c	\
			 benchhashserver.c

LIBOBJS = $(LIBSRC:.c=.o)
BINS = testhashtable benchhashtable hashserver2 benchhashserver

all: $(BINS)

hashserver2 testhashtable benchhashtable: %: %.o $(LIBOBJS)
	gcc -o $@ $^ $(LFLAGS)

benchhashtable: LFLAGS += -lprofiler

benchhashserver: %: %.o util.o hashclient.o
	gcc -o $@ $^ -lmemcached $(LFLAGS)

%.P : %.c
				$(MAKEDEPEND)
				@sed 's/\($*\)\.o[ :]*/\1.o $@ : /g' < $*.d > $@; \
					rm -f $*.d; [ -s $@ ] || rm -f $@

include $(SRCS:.c=.P)

.PHONY: clean
clean: 
	rm -f $(BINS) *.o *.P

