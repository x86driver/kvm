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

CFLAGS = -DCONFIG_X86_64 -DCONFIG_X86
CFLAGS += -I./ -Iinclude -Ix86/include
CFLAGS += -Wall
CFLAGS += -fno-strict-aliasing
CFLAGS += -g
#CFLAGS += -O2

BIOS_CFLAGS += -m32
BIOS_CFLAGS += -march=i386
BIOS_CFLAGS += -mregparm=3

BIOS_CFLAGS += -fno-stack-protector
BIOS_CFLAGS += -fno-pic

x86/bios.o: x86/bios/bios.bin x86/bios/bios-rom.h

x86/bios/bios.bin.elf: x86/bios/entry.S x86/bios/e820.c x86/bios/int10.c x86/bios/int15.c x86/bios/rom.ld.S
	$(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/memcpy.c -o x86/bios/memcpy.o
	$(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/e820.c -o x86/bios/e820.o
	$(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/int10.c -o x86/bios/int10.o
	$(CC) -include code16gcc.h $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/int15.c -o x86/bios/int15.o
	$(CC) $(CFLAGS) $(BIOS_CFLAGS) -c x86/bios/entry.S -o x86/bios/entry.o
	$(LD) -T x86/bios/rom.ld.S -o x86/bios/bios.bin.elf x86/bios/memcpy.o x86/bios/entry.o x86/bios/e820.o x86/bios/int10.o x86/bios/int15.o

x86/bios/bios.bin: x86/bios/bios.bin.elf
	$(OBJCOPY) -O binary -j .text x86/bios/bios.bin.elf x86/bios/bios.bin

x86/bios/bios-rom.o: x86/bios/bios-rom.S x86/bios/bios.bin x86/bios/bios-rom.h
	$(CC) -c $(CFLAGS) x86/bios/bios-rom.S -o x86/bios/bios-rom.o

x86/bios/bios-rom.h: x86/bios/bios.bin.elf
	cd x86/bios && sh gen-offsets.sh > bios-rom.h && cd ..

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

bios.o:x86/bios.c x86/bios/bios-rom.h
	gcc -g -Wall -c -o $@ $<

rbtree.o:rbtree.c
	gcc $(CFLAGS) -c -o $@ $<

mptable.o:mptable.c
	gcc $(CFLAGS) -c -o $@ $<

term.o:term.c
	gcc $(CFLAGS) -c -o $@ $<

serial.o:serial.c
	gcc $(CFLAGS) -c -o $@ $<

kvm.o:kvm.c
	gcc $(CFLAGS) -c -o $@ $<

kvm: kvm.o rbtree.o x86/bios.o x86/bios/bios-rom.o term.o serial.o mptable.o
	gcc -g -Wall -o $@ $^

$(TARGET_TEST): test.o
	$(OBJCOPY) -O binary test.o $(TARGET_TEST)

test.o: test.S
	$(AS) $(ASFLAGS) test.S -o test.o

clean:
	rm -f *.o $(TARGETS) x86/bios/*.o x86/bios/*.bin x86/bios/*.elf x86/bios/bios-rom.h

.PHONY: all clean
