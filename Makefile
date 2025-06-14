CC = gcc
CFLAGS = -Wall -Wextra -pedantic -std=c99

SRCS = src/main.c
OBJS = $(SRCS:.c=.o)

TARGET = editor

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET) 