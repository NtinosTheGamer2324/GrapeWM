CC = gcc
CFLAGS = -Wall -Wextra -O2 -Iinclude $(shell pkg-config --cflags freetype2)
LDFLAGS = -lX11 -lXft -lfreetype -lfontconfig $(shell pkg-config --libs freetype2)

# Automatically find all .c files in src/
SRC := $(shell find src -name '*.c')

# Convert src/foo.c -> build/foo.o
OBJ := $(patsubst src/%.c,build/%.o,$(SRC))

TARGET = grape-wm

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# Compile rule with auto dir creation
build/%.o: src/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf build $(TARGET)
