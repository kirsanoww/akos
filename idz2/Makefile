CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -pthread
COMMON = src/main.c src/config.c src/io.c src/model.c
HEADERS = src/center.h src/sync.h
SANITIZERS ?= address,undefined
BUILD ?= build
SANITIZE_DIR ?= build/sanitize

.PHONY: all test run sanitize tsan clean
all: $(BUILD)/sorting_center

$(BUILD)/sorting_center: $(COMMON) src/sync_sem.c $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(COMMON) src/sync_sem.c $(LDFLAGS) $(LDLIBS) -o $@

run: all
	./$(BUILD)/sorting_center --delay-ms 100

$(BUILD)/sorting_center_fault: $(COMMON) src/sync_sem.c tests/fail_create.c $(HEADERS)
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) -Dpthread_create=test_pthread_create $(CFLAGS) $(COMMON) src/sync_sem.c tests/fail_create.c $(LDFLAGS) $(LDLIBS) -o $@

test: all $(BUILD)/sorting_center_fault
	python3 tests/test_simulation.py ./$(BUILD)/sorting_center ./$(BUILD)/sorting_center_fault

sanitize:
	$(MAKE) BUILD=$(SANITIZE_DIR) CFLAGS='-std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -fsanitize=$(SANITIZERS) -fno-omit-frame-pointer' LDFLAGS='-fsanitize=$(SANITIZERS)' -B test

tsan:
	$(MAKE) SANITIZERS=thread SANITIZE_DIR=build/tsan sanitize

clean:
	rm -f $(BUILD)/sorting_center $(BUILD)/sorting_center_fault build/sanitize/sorting_center build/sanitize/sorting_center_fault build/tsan/sorting_center build/tsan/sorting_center_fault
