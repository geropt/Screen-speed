CC ?= cc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O1
SANITIZE ?= -fsanitize=address,undefined
SRC := ../main
# El framing del Ruptela se movió a un componente compartido con el HUD (P02a).
SHARED := ../../components/ruptela_framing

.PHONY: test clean

test: test_ruptela_io_parser test_ruptela_proto
	./test_ruptela_io_parser
	./test_ruptela_proto

test_ruptela_io_parser: test_ruptela_io_parser.c $(SHARED)/ruptela_io_parser.c
	$(CC) $(CFLAGS) $(SANITIZE) -I$(SRC) -I$(SHARED)/include -o $@ $^

test_ruptela_proto: test_ruptela_proto.c $(SHARED)/ruptela_proto.c
	$(CC) $(CFLAGS) $(SANITIZE) -I$(SRC) -I$(SHARED)/include -o $@ $^

clean:
	rm -f test_ruptela_io_parser test_ruptela_proto
	rm -rf test_ruptela_io_parser.dSYM test_ruptela_proto.dSYM
