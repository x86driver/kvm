CC = g++
AS = as
OBJCOPY = objcopy
CFLAGS = -Wall -g
ASFLAGS =

TARGET_MAIN = main
TARGET_INPUT = input.bin
TARGET_TEST = test.bin

all: $(TARGET_MAIN) $(TARGET_INPUT) $(TARGET_TEST)

$(TARGET_MAIN): main.o
	$(CC) main.o -o $(TARGET_MAIN)

main.o: main.cpp
	$(CC) $(CFLAGS) -c main.cpp -o main.o

$(TARGET_INPUT): input.o

input.o: input.S
	nasm -f bin input.S -o input.bin

$(TARGET_TEST): test.o
	$(OBJCOPY) -O binary test.o $(TARGET_TEST)

test.o: test.S
	$(AS) $(ASFLAGS) test.S -o test.o

clean:
	rm -f *.o $(TARGET_MAIN) $(TARGET_INPUT) $(TARGET_TEST)

.PHONY: all clean
