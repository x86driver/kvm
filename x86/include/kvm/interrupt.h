#ifndef _X86_INTERRUPT_H_
#define _X86_INTERRUPT_H_

#include <stdint.h>
#include <kvm/bios.h>

struct real_intr_desc {
	uint16_t offset;
	uint16_t segment;
} __attribute__((packed));

#define REAL_SEGMENT_SHIFT	4
#define REAL_SEGMENT(addr)	((addr) >> REAL_SEGMENT_SHIFT)
#define REAL_OFFSET(addr)	((addr) & ((1 << REAL_SEGMENT_SHIFT) - 1))
#define REAL_INTR_SIZE		(REAL_INTR_VECTORS * sizeof(struct real_intr_desc))

struct interrupt_table {
	struct real_intr_desc entries[REAL_INTR_VECTORS];
};

#endif