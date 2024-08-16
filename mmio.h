#ifndef _MMIO_H_
#define _MMIO_H_

#include <stdint.h>
#include <pthread.h>
#include "rbtree.h"

#define RB_ROOT	{ NULL, }
#define RB_INT_INIT(l, h) \
    (struct rb_int_node){.low = l, .high = h}
#define rb_int(n)	rb_entry(n, struct rb_int_node, node)
#define rb_int_start(n)	((n)->low)
#define rb_int_end(n)	((n)->low + (n)->high - 1)
#define mmio_node(n) rb_entry(n, struct mmio_mapping, node)

typedef void (*mmio_handler_fn)(struct kvm_cpu *vcpu, uint64_t addr, uint8_t *data,
                uint32_t len, uint8_t is_write, void *ptr);

int kvm__register_iotrap(struct kvm *kvm, uint64_t phys_addr, uint64_t phys_addr_len,
             mmio_handler_fn mmio_fn, void *ptr,
             unsigned int flags);

struct rb_int_node {
    struct rb_node	node;
    uint64_t		low;
    uint64_t		high;
};

struct mmio_mapping {
    struct rb_int_node	node;
    mmio_handler_fn		mmio_fn;
    void			*ptr;
    uint32_t			refcount;
    int			remove;
};

#endif
