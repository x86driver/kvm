CC = g++
AS = as
OBJCOPY = objcopy
CFLAGS = -Wall -g
ASFLAGS =

TARGET_MAIN = main
TARGET_INPUT = input.bin
TARGET_TEST = test.bin

TARGETS = $(TARGET_MAIN) $(TARGET_INPUT) $(TARGET_TEST) $(TARGET_INPUT_TERRUPT) kvm boot.bin

all: $(TARGETS)

$(TARGET_MAIN): main.o
	$(CC) main.o -o $(TARGET_MAIN)

main.o: main.cpp
	$(CC) $(CFLAGS) -c main.cpp -o main.o

$(TARGET_INPUT): input.o

input.o: input.S
	nasm -f bin input.S -o input.bin

boot.bin: boot.S
	nasm -f bin boot.S -o boot.bin

kvm: kvm.c
	gcc -Wall kvm.c -o kvm

$(TARGET_TEST): test.o
	$(OBJCOPY) -O binary test.o $(TARGET_TEST)

test.o: test.S
	$(AS) $(ASFLAGS) test.S -o test.o

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean
