CC      ?= cc
CFLAGS  ?= -O2 -std=c99 -Wall -Wextra -D_FILE_OFFSET_BITS=64 -D_POSIX_C_SOURCE=200809L
LDLIBS   = -lm -pthread

SRC = src/isum.c src/bitio.c src/rle.c src/huff.c src/lzi.c src/recipe.c src/block.c src/archive.c src/util.c
OBJ = $(SRC:.c=.o)
LIBOBJ = $(filter-out src/main.o,$(OBJ))

all: isc libisc.a

isc: src/main.o $(OBJ)
	$(CC) $(CFLAGS) -o $@ src/main.o $(OBJ) $(LDLIBS)

libisc.a: $(LIBOBJ)
	ar rcs $@ $^

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

test: isc
	./tests/roundtrip.sh

bench: isc
	./bench/bench.sh /tmp/isc-bench-corpus

clean:
	rm -f src/*.o isc libisc.a

.PHONY: all test bench clean
