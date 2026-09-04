CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1
SANITIZE ?= -fsanitize=address,undefined
SRC := ../main

.PHONY: test clean

test: test_ruptela_io_parser test_ruptela_proto
	./test_ruptela_io_parser
	./test_ruptela_proto

test_ruptela_io_parser: test_ruptela_io_parser.c $(SRC)/ruptela_io_parser.c
	$(CC) $(CFLAGS) $(SANITIZE) -I$(SRC) -o $@ $^

test_ruptela_proto: test_ruptela_proto.c $(SRC)/ruptela_proto.c
	$(CC) $(CFLAGS) $(SANITIZE) -I$(SRC) -o $@ $^

clean:
	rm -f test_ruptela_io_parser test_ruptela_proto
