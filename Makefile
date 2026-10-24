CC ?= cc
CFLAGS ?= -O2
CFLAGS += -std=c11 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -Wall -Wextra -Wpedantic -Wshadow -Werror
PREFIX ?= /usr/local

SOURCES := $(wildcard src/*.c)
OBJECTS := $(SOURCES:src/%.c=build/%.o)

glassh: $(OBJECTS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^

build/%.o: src/%.c src/glassh.h | build
	$(CC) $(CFLAGS) -c -o $@ $<

build:
	mkdir -p build

test: glassh
	tests/run.sh ./glassh

format:
	clang-format -i src/*.c src/*.h

check-format:
	clang-format --dry-run --Werror src/*.c src/*.h

install: glassh
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 glassh $(DESTDIR)$(PREFIX)/bin/glassh

clean:
	rm -rf build glassh

.PHONY: test format check-format install clean
