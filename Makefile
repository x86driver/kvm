CC = g++
AS = as
OBJCOPY = objcopy
CFLAGS = -Wall -g
ASFLAGS =

TARGET = program
OBJS = main.o test.o

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) -o $(TARGET)

main.o: main.cpp
	$(CC) $(CFLAGS) -c main.cpp -o main.o

test.o: test.S
	$(AS) $(ASFLAGS) test.S -o test.o
	$(OBJCOPY) -O binary test.o test.bin

clean:
	rm -f $(OBJS) $(TARGET) test.bin

.PHONY: all clean
