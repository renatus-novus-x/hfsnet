CROSS = m68k-xelf-
CC = $(CROSS)gcc
CFLAGS = -Wall -O2 -g

TARGET = hfsnet.x
OBJS = head.o hfsnet.o
LIBS = -lsocket

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

%.o: %.S
	$(CC) -c -o $@ $<

clean:
	-rm -f $(OBJS) $(TARGET) *.elf

.PHONY: all clean
