# Default compiler and flags
CC ?= $(CROSS_COMPILE)gcc
CFLAGS ?= -Wall -Wextra -Werror -g
LDFLAGS ?= -pthread -lrt

TARGET = aesdsocket
OBJS = aesdsocket.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS) *.o /var/tmp/aesdsocketdata

.PHONY: all clean
