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

# rm -f e portavel para todo shell capaz de compilar este projeto no Windows
# (MinGW, MSYS2, Git Bash, WSL). A variante anterior decidia por $(OS), que vale
# Windows_NT tambem no Git Bash, onde 'del' e 'nul' nao existem: o alvo abortava
# com erro 127 e criava um arquivo literal chamado nul.
clean:
	rm -f $(OBJ) $(TARGET) $(TEST_BIN)

.PHONY: all test clean
