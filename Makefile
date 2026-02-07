CC=gcc
CFLAGS=-Wall -Wextra -O2 -Iinclude
BIN=bin
SRC=src

all: $(BIN)/server $(BIN)/client

$(BIN)/server: $(SRC)/server.c $(SRC)/board.c include/common.h include/board.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/server.c $(SRC)/board.c -pthread

$(BIN)/client: $(SRC)/client.c include/common.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ $(SRC)/client.c

$(BIN):
	mkdir -p $(BIN)

clean:
	rm -rf $(BIN)

.PHONY: all clean
