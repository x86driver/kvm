#ifndef _KVM_H
#define _KVM_H

#include <stdint.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <linux/kvm.h>
#include "list.h"

#define KVM_API_VERSION 12
#define KVM_32BIT_MAX_MEM_SIZE (1ULL << 32)
#define KVM_32BIT_GAP_SIZE (768 << 20)
#define KVM_32BIT_GAP_START (KVM_32BIT_MAX_MEM_SIZE - KVM_32BIT_GAP_SIZE)

#define RAM_SIZE (2ULL << 30) /* 2GB */

#define BIOS_ENTRY_SIZE(name) (name##_end - name)

#define BIOS_OFFSET__bios_int10 0x00000040
#define BIOS_OFFSET__bios_int10_end 0x0000007f
#define BIOS_OFFSET__bios_int15 0x00000080
#define BIOS_OFFSET__bios_int15_end 0x000000b8
#define BIOS_OFFSET__bios_intfake 0x00000030
#define BIOS_OFFSET__bios_intfake_end 0x00000038
#define BIOS_OFFSET____CALLER_CLOBBER 0x000000c0
#define BIOS_OFFSET____CALLER_SP 0x000000bc
#define BIOS_OFFSET____CALLER_SS 0x000000b8
#define BIOS_OFFSET__e820_query_map 0x000000c4
#define BIOS_OFFSET__int10_handler 0x00000168
#define BIOS_OFFSET__int15_handler 0x00000294
#define BIOS_OFFSET____locals 0x000000b8
#define BIOS_OFFSET____locals_end 0x000000c4
#define BIOS_OFFSET__memcpy16 0x00000000

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

static char kern_cmdline[2048] = "noapic noacpi pci=conf1 reboot=k panic=1 i8042.direct=1 i8042.dumbkbd=1 i8042.nopnp=1 earlyprintk=serial i8042.noaux=1 console=ttyS0 root=/dev/vda rw ";

#ifndef BIOS_EXPORT_H_
#define BIOS_EXPORT_H_

extern char bios_rom[0];
extern char bios_rom_end[0];

#define bios_rom_size		(bios_rom_end - bios_rom)

#endif /* BIOS_EXPORT_H_ */

static inline void kvm__set_thread_name(const char *name) {
    prctl(15, name);
}

struct real_intr_desc {
    uint16_t offset;
    uint16_t segment;
} __attribute__((packed));

struct e820entry {
    uint64_t addr;     /* start of memory segment */
    uint64_t size;     /* size of memory segment */
    uint32_t type;     /* type of memory segment */
} __attribute__((packed));

struct e820map {
    uint32_t nr_map;
        struct e820entry map[128];
};

struct irq_handler {
    unsigned long		address;
    unsigned int		irq;
    void			*handler;
    size_t			size;
};

struct interrupt_table {
    struct real_intr_desc entries[256];
};

struct kvm_mem_bank {
    struct list_head	list;
    uint64_t			guest_phys_addr;
    void			*host_addr;
    uint64_t			size;
    uint32_t			slot;
};

struct kvm {
    int sys_fd;      /* For system ioctls(), i.e. /dev/kvm */
    int vm_fd;       /* For VM ioctls() */
    char *kernel_filename;    /* Filename of kernel**/
    char *initrd_filename;

    uint32_t ram_slots;    /* for KVM_SET_USER_MEMORY_REGION */
    uint64_t ram_size;		/* Guest memory size, in bytes */
    void *ram_start;
    uint64_t ram_pagesize;
    pthread_mutex_t mutex;

    int nrcpus; /* Number of cpus to run */
    struct kvm_cpu **cpus;

    uint32_t mem_slots; /* for KVM_SET_USER_MEMORY_REGION */
    struct list_head mem_banks;

    struct interrupt_table interrupt_table;
};

struct kvm_cpu {
    pthread_t thread;		/* VCPU thread */
    unsigned long cpu_id;
    struct kvm *kvm;		/* parent KVM */
    int	vcpu_fd;            /* For VCPU ioctls() */
    struct kvm_run *kvm_run;
    struct kvm_regs  regs;
    struct kvm_sregs sregs;
};

static struct irq_handler bios_irq_handlers[] = {
    DEFINE_BIOS_IRQ_HANDLER(0x10, bios_int10),
    DEFINE_BIOS_IRQ_HANDLER(0x15, bios_int15),
};

void kvm__arch_read_term(struct kvm *kvm);
void kvm__irq_line(struct kvm *kvm, int irq, int level);

#endif
