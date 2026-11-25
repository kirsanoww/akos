CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
BUILD ?= build
.PHONY: all test sanitize clean
all: $(BUILD)/center_server $(BUILD)/center_worker $(BUILD)/center_observer
$(BUILD)/center_server: src/server.c src/model.c src/config.c src/net.c src/center.h src/model.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/server.c src/model.c src/config.c src/net.c $(LDFLAGS) -o $@
$(BUILD)/center_worker: src/client.c src/net.c src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) src/client.c src/net.c $(LDFLAGS) -o $@
$(BUILD)/center_observer: src/client.c src/net.c src/center.h
	mkdir -p $(BUILD)
	$(CC) $(CPPFLAGS) $(CFLAGS) -DOBSERVER=1 src/client.c src/net.c $(LDFLAGS) -o $@
test: all
	python3 tests/test_network.py $(BUILD)
sanitize:
	$(MAKE) BUILD=build/sanitize CFLAGS='-std=c11 -O1 -g -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined -fno-omit-frame-pointer' LDFLAGS='-fsanitize=address,undefined' test
clean:
	rm -rf build
