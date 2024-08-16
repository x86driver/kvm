#ifndef _KVM_H
#define _KVM_H

#include <stdint.h>
#include <pthread.h>
#include <sys/prctl.h>
#include <linux/kvm.h>
#include "list.h"
#include <kvm/interrupt.h>

#define KVM_API_VERSION 12
#define KVM_32BIT_MAX_MEM_SIZE (1ULL << 32)
#define KVM_32BIT_GAP_SIZE (768 << 20)
#define KVM_32BIT_GAP_START (KVM_32BIT_MAX_MEM_SIZE - KVM_32BIT_GAP_SIZE)

#define RAM_SIZE (2ULL << 30) /* 2GB */

#ifndef BIOS_EXPORT_H_
#define BIOS_EXPORT_H_

extern char bios_rom[0];
extern char bios_rom_end[0];

#define bios_rom_size		(bios_rom_end - bios_rom)

struct kvm;
void kvm__setup_bios(struct kvm *kvm);

#endif /* BIOS_EXPORT_H_ */

static inline void kvm__set_thread_name(const char *name) {
    prctl(15, name);
}

struct irq_handler {
    unsigned long		address;
    unsigned int		irq;
    void			*handler;
    size_t			size;
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

void kvm__arch_read_term(struct kvm *kvm);
void kvm__irq_line(struct kvm *kvm, int irq, int level);
void *guest_flat_to_host(struct kvm *kvm, uint64_t offset);
uint64_t host_to_guest_flat(struct kvm *kvm, void *ptr);

#endif
