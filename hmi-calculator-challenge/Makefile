CC = gcc
CFLAGS = -Wall -Wextra -I./inc

SRC = main.c src/math_ops.c src/hmi.c src/logger.c
OBJ = $(SRC:.c=.o)
TARGET = dynamox_app.exe

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

$(OBJ): $(wildcard inc/*.h)

clean:
ifeq ($(OS),Windows_NT)
	del /Q $(subst /,\,$(OBJ) $(TARGET)) 2>nul
else
	rm -f $(OBJ) $(TARGET)
endif
