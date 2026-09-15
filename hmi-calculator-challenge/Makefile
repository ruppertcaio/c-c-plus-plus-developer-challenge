CC = gcc
CFLAGS = -Wall -Wextra -I./inc

SRC = main.c src/math_ops.c src/hmi.c src/logger.c
OBJ = $(SRC:.c=.o)
TARGET = dynamox_app.exe

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ): $(wildcard inc/*.h)

# Testes unitários: apenas math_ops.c (sem main.c/hmi.c/logger.c).
TEST_SRC = tests/test_math.c src/math_ops.c
TEST_BIN = tests/test_math.exe

test: $(TEST_BIN)
	./$(TEST_BIN)

$(TEST_BIN): $(TEST_SRC) inc/math_ops.h
	$(CC) $(CFLAGS) -o $@ $(TEST_SRC)

clean:
ifeq ($(OS),Windows_NT)
	del /Q $(subst /,\,$(OBJ) $(TARGET) $(TEST_BIN)) 2>nul
else
	rm -f $(OBJ) $(TARGET) $(TEST_BIN)
endif

.PHONY: all test clean
