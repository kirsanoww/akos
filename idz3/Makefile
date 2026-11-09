CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
LDLIBS = -pthread -lrt
BUILD ?= build
COMMON = src/main.c src/config.c src/io.c src/ipc.c src/model.c src/controller.c

.PHONY: all test sanitize clean
all: $(BUILD)/center_named $(BUILD)/center_unnamed $(BUILD)/center_session $(BUILD)/center_worker

$(BUILD)/center_named: $(COMMON) src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=1 $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/center_unnamed: $(COMMON) src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=2 $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/center_session: $(COMMON) src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=3 $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/center_worker: $(COMMON) src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=4 $(COMMON) $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/center_fault: $(COMMON) src/center.h tests/fail_fork.c
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=1 -Dfork=test_fork $(COMMON) tests/fail_fork.c $(LDFLAGS) $(LDLIBS) -o $@

$(BUILD)/center_fault_unnamed: $(COMMON) src/center.h tests/fail_fork.c
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DCENTER_MODE=2 -Dfork=test_fork $(COMMON) tests/fail_fork.c $(LDFLAGS) $(LDLIBS) -o $@

test: all $(BUILD)/center_fault $(BUILD)/center_fault_unnamed
	python3 tests/test_simulation.py $(BUILD)

sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test

clean:
	rm -rf build
