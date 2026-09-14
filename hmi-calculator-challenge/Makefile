CC = gcc
CFLAGS = -Wall -Wextra -I./inc

SRC = src/main.c src/math_ops.c src/hmi.c
OBJ = $(SRC:.c=.o)
TARGET = dynamox_app.exe

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f src/*.o $(TARGET)