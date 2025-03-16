#
# file:        Makefile - project 2
# description: compile, link with pthread and zlib (crc32) libraries
#

LDLIBS=-lz -lpthread
CFLAGS=-ggdb3 -Wall -Wno-format-overflow
EXES = dbserver dbtest

all: $(EXES)

dbtest: dbtest.c proj2.h
	gcc $(CFLAGS) -o dbtest dbtest.c $(LDLIBS)

dbserver: dbserver.c proj2.h
	gcc $(CFLAGS) -o dbserver dbserver.c $(LDLIBS)

clean:
	rm -f $(EXES) *.o data.[0-9]*
