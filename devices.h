#ifndef _DEVICES_H_
#define _DEVICES_H_

#include "rbtree.h"

enum device_bus_type {
    DEVICE_BUS_PCI,
    DEVICE_BUS_MMIO,
    DEVICE_BUS_IOPORT,
    DEVICE_BUS_MAX,
};

struct device_header {
    enum device_bus_type	bus_type;
    void			*data;
    int			dev_num;
    struct rb_node		node;
};

struct device_header *device__first_dev(enum device_bus_type bus_type);
struct device_header *device__next_dev(struct device_header *dev);

#endif