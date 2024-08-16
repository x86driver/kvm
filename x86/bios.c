#ifndef _X86_BIOS_H_
#define _X86_BIOS_H_

#include <stdio.h>
#include <string.h>
#include "kvm.h"
#include <kvm/boot-protocol.h>
#include <kvm/e820.h>
#include "bios/bios-rom.h"

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

#define BIOS_IRQ_PA_ADDR(name)	(0x000f0000 + BIOS_OFFSET__##name)
#define BIOS_IRQ_FUNC(name)	((char *)&bios_rom[BIOS_OFFSET__##name])
#define BIOS_IRQ_SIZE(name)	(BIOS_ENTRY_SIZE(BIOS_OFFSET__##name))

#define DEFINE_BIOS_IRQ_HANDLER(_irq, _handler)			\
    {							\
        .irq		= _irq,				\
        .address	= BIOS_IRQ_PA_ADDR(_handler),	\
        .handler	= BIOS_IRQ_FUNC(_handler),	\
        .size		= BIOS_IRQ_SIZE(_handler),	\
    }

static struct irq_handler bios_irq_handlers[] = {
    DEFINE_BIOS_IRQ_HANDLER(0x10, bios_int10),
    DEFINE_BIOS_IRQ_HANDLER(0x15, bios_int15),
};

void interrupt_table__setup(struct interrupt_table *itable, struct real_intr_desc *entry) {
    unsigned int i;

    for (i = 0; i < 256; i++)
        itable->entries[i] = *entry;
}

void interrupt_table__set(struct interrupt_table *itable,
                struct real_intr_desc *entry, unsigned int num) {
    if (num < 256)
        itable->entries[num] = *entry;
}


void interrupt_table__copy(struct interrupt_table *itable, void *dst, unsigned int size)
{
    if (size < sizeof(itable->entries))
        perror("An attempt to overwrite host memory");

    memcpy(dst, itable->entries, sizeof(itable->entries));
}

static void setup_irq_handler(struct kvm *kvm, struct irq_handler *handler) {
    struct real_intr_desc intr_desc;
    void *p;

    p = guest_flat_to_host(kvm, handler->address);
    memcpy(p, handler->handler, handler->size);

    intr_desc = (struct real_intr_desc) {
        .segment	= (0x000f0000 >> 4),
        .offset		= handler->address - 0x000f0000,
    };

    interrupt_table__set(&kvm->interrupt_table, &intr_desc, handler->irq);
}

static void e820_setup(struct kvm *kvm) {
    struct e820map *e820;
    struct e820entry *mem_map;
    unsigned int i = 0;

    e820 = guest_flat_to_host(kvm, E820_MAP_START);
    mem_map = e820->map;

	mem_map[i++]	= (struct e820entry) {
		.addr		= REAL_MODE_IVT_BEGIN,
		.size		= EBDA_START - REAL_MODE_IVT_BEGIN,
		.type		= E820_RAM,
	};
	mem_map[i++]	= (struct e820entry) {
		.addr		= EBDA_START,
		.size		= VGA_RAM_BEGIN - EBDA_START,
		.type		= E820_RESERVED,
	};
	mem_map[i++]	= (struct e820entry) {
		.addr		= MB_BIOS_BEGIN,
		.size		= MB_BIOS_SIZE,
		.type		= E820_RESERVED,
	};
	if (kvm->ram_size < KVM_32BIT_GAP_START) {
		mem_map[i++]	= (struct e820entry) {
			.addr		= BZ_KERNEL_START,
			.size		= kvm->ram_size - BZ_KERNEL_START,
			.type		= E820_RAM,
		};
	} else {
		mem_map[i++]	= (struct e820entry) {
			.addr		= BZ_KERNEL_START,
			.size		= KVM_32BIT_GAP_START - BZ_KERNEL_START,
			.type		= E820_RAM,
		};
		mem_map[i++]	= (struct e820entry) {
			.addr		= KVM_32BIT_MAX_MEM_SIZE,
			.size		= kvm->ram_size - KVM_32BIT_MAX_MEM_SIZE,
			.type		= E820_RAM,
		};
	}

    if (i > 128)
        perror("BUG too big");

    e820->nr_map = i;

}

static void setup_vga_rom(struct kvm *kvm) {
    uint16_t *mode;
    void *p;

    p = guest_flat_to_host(kvm, VGA_ROM_OEM_STRING);
    memset(p, 0, VGA_ROM_OEM_STRING_SIZE);
    strncpy(p, "KVM VESA", VGA_ROM_OEM_STRING_SIZE);

    mode = guest_flat_to_host(kvm, VGA_ROM_MODES);
    mode[0] = 0x0112;
    mode[1] = 0xffff;
}

void kvm__setup_bios(struct kvm *kvm) {
    unsigned long address = MB_BIOS_BEGIN;
    struct real_intr_desc intr_desc;
    void *p;
/*
0xFFFFFFFF  ------------------------- 4 G
           |                         |
           |          ....           |
            ------------------------- 16 G
           |                         |
           |          ....           |
  0x100000  ------------------------- 1 M
           |     ROM BIOS Sector     |
   0xF0000  -------------------------
           |    Others BIOS Sector   |
   0xE0000  -------------------------
           |   Memory of Other ROM   |
   0xC7FFF  -------------------------
           |      VGA ROM BIOS       |
   0xC0000  ------------------------- 768 K
           |      Display Buffer     |
   0xA0000  ------------------------- 640 K
           |                         |
           |          ....           |
   0x00500  -------------------------
           |        BIOS Data        |
   0x00400  -------------------------
           |            IVT          |
   0x00000  ------------------------- 0

*/

    p = guest_flat_to_host(kvm, BDA_START);
    memset(p, 0, BDA_SIZE);

    p = guest_flat_to_host(kvm, EBDA_START);
    memset(p, 0, EBDA_SIZE);

    p = guest_flat_to_host(kvm, MB_BIOS_BEGIN);
    memset(p, 0, MB_BIOS_SIZE);

    p = guest_flat_to_host(kvm, VGA_ROM_BEGIN);
    memset(p, 0, VGA_ROM_SIZE);

    p = guest_flat_to_host(kvm, MB_BIOS_BEGIN);
    memcpy(p, bios_rom, bios_rom_size);

    e820_setup(kvm);

    setup_vga_rom(kvm);

    address = BIOS_IRQ_PA_ADDR(bios_intfake);
    intr_desc = (struct real_intr_desc) {
        .segment = REAL_SEGMENT(MB_BIOS_BEGIN),
        .offset  = address - MB_BIOS_BEGIN,
    };

    interrupt_table__setup(&kvm->interrupt_table, &intr_desc);

    for (int i = 0; i < ARRAY_SIZE(bios_irq_handlers); i++)
        setup_irq_handler(kvm, &bios_irq_handlers[i]);

    // The IVT stores in 0 of physical address
    p = guest_flat_to_host(kvm, 0);
    interrupt_table__copy(&kvm->interrupt_table, p, 1024);
}

#endif