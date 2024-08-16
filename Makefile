CC = gcc
AS = as
OBJCOPY = objcopy
CFLAGS = -Wall -g
ASFLAGS =

TARGET_INPUT = input.bin
TARGET_TEST = test.bin
TARGET_INPUT_TERRUPT = input_interrupt.bin

TARGETS = $(TARGET_INPUT) $(TARGET_TEST) $(TARGET_INPUT_TERRUPT) kvm boot.bin pit.bin bios.bin apic.bin

all: $(TARGETS)

$(TARGET_INPUT): input.S
	nasm -f bin input.S -o input.bin

$(TARGET_INPUT_TERRUPT): input_interrupt.S
	nasm -f bin input_interrupt.S -o input_interrupt.bin

bios.bin: bios.S
	nasm -f bin bios.S -o bios.bin

pit.bin: pit.S
	nasm -f bin pit.S -o pit.bin

apic.bin: apic.S
	nasm -f bin apic.S -o apic.bin

boot.bin: boot.S
	nasm -f bin boot.S -o boot.bin

rbtree.o:rbtree.c
	gcc -g -Wall -c -o $@ $<

kvm.o:kvm.c
	gcc -g -Wall -c -o $@ $<

kvm: kvm.o rbtree.o bios-rom.o
	gcc -g -Wall -o $@ $^

$(TARGET_TEST): test.o
	$(OBJCOPY) -O binary test.o $(TARGET_TEST)

test.o: test.S
	$(AS) $(ASFLAGS) test.S -o test.o

clean:
	rm -f *.o $(TARGETS)

.PHONY: all clean
