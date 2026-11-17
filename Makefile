CC      ?= gcc
CFLAGS  ?= -O2 -Wall -Wextra -Wpedantic -std=gnu11
BIN     := bin

all: $(BIN)/logpipe $(BIN)/logsingle $(BIN)/loggen

$(BIN):
	mkdir -p $(BIN)

$(BIN)/logpipe: src/logpipe.c src/logcore.c src/logcore.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/logpipe.c src/logcore.c

$(BIN)/logsingle: src/logsingle.c src/logcore.c src/logcore.h | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/logsingle.c src/logcore.c

$(BIN)/loggen: src/loggen.c | $(BIN)
	$(CC) $(CFLAGS) -o $@ src/loggen.c

test: all
	bash tests/run_tests.sh

bench: all
	python3 scripts/bench.py

clean:
	rm -rf $(BIN)

.PHONY: all test bench clean
