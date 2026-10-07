CC = cc
CPPFLAGS = -D_POSIX_C_SOURCE=200809L
CFLAGS = -std=c11 -O2 -Wall -Wextra -Wpedantic -Werror
SANITIZERS ?= address,undefined
TARGET = build/sorting_center
SOURCES = src/main.c src/config.c src/io.c src/simulation.c

.PHONY: all run test sanitize clean
all: $(TARGET)

$(TARGET): $(SOURCES) src/center.h
	mkdir -p build
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SOURCES) $(LDFLAGS) -o $@

run: $(TARGET)
	./$(TARGET) --delay-ms 100

test: $(TARGET)
	python3 tests/test_simulation.py ./$(TARGET)
	python3 tests/test_simulation.py ./$(TARGET) --check

sanitize:
	mkdir -p build
	$(CC) $(CPPFLAGS) -std=c11 -g -O1 -Wall -Wextra -Wpedantic -Werror -fsanitize=$(SANITIZERS) -fno-omit-frame-pointer $(SOURCES) -o build/sorting_center_sanitize
	python3 tests/test_simulation.py ./build/sorting_center_sanitize
	python3 tests/test_simulation.py ./build/sorting_center_sanitize --check

clean:
	rm -f build/sorting_center build/sorting_center_sanitize build/sorting_center_ubsan
