CC=gcc
TARGET=ni

$(TARGET): main.c
	$(CC) main.c -o $(TARGET) -Wall -Wextra -pedantic -std=c99

.PHONY: clean
clean:
	rm $(TARGET)
