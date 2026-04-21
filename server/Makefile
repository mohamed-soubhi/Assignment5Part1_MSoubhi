CC      ?= gcc
CFLAGS  = -Wall -Wextra -g
TARGET  = aesdsocket
SRCDIR  = .

.PHONY: all default clean

all: $(TARGET)
default: all

$(TARGET): $(SRCDIR)/aesdsocket.c
	$(CC) $(CFLAGS) -o $(SRCDIR)/$(TARGET) $<

clean:
	rm -f $(SRCDIR)/$(TARGET)
