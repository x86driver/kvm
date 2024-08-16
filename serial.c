#include <stdint.h>
#include <linux/serial_reg.h>
#include <asm/byteorder.h>
#include <linux/byteorder/little_endian.h>
#include "rbtree.h"
#include "kvm.h"
#include "term.h"
#include "devices.h"
#include "mmio.h"

#define serial_iobase_0		0x3f8
#define serial_iobase_1		0x2f8
#define serial_iobase_2		0x3e8
#define serial_iobase_3		0x2e8
#define serial_irq_0		4
#define serial_irq_1		3
#define serial_irq_2		4
#define serial_irq_3		3
#define SERIAL_REGS_SETTING \
    .iir			= UART_IIR_NO_INT, \
    .lsr			= UART_LSR_TEMT | UART_LSR_THRE, \
    .msr			= UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS, \
    .mcr			= UART_MCR_OUT2,
#define serial_iobase(nr)	serial_iobase_##nr
#define serial_irq(nr)		serial_irq_##nr
#define SERIAL8250_BUS_TYPE	DEVICE_BUS_IOPORT

static int sysrq_pending;

static inline uint8_t ioport__read8(uint8_t *data)
{
    return *data;
}
/* On BE platforms, PCI I/O is byteswapped, i.e. LE, so swap back. */
static inline uint16_t ioport__read16(uint16_t *data)
{
    return __le16_to_cpu(*data);
}

static inline uint32_t ioport__read32(uint32_t *data)
{
    return __le32_to_cpu(*data);
}

static inline void ioport__write8(uint8_t *data, uint8_t value)
{
    *data		 = value;
}

static inline void ioport__write16(uint16_t *data, uint16_t value)
{
    *data		 = __cpu_to_le16(value);
}

static inline void ioport__write32(uint32_t *data, uint32_t value)
{
    *data		 = __cpu_to_le32(value);
}

struct device_bus {
    struct rb_root	root;
    int		dev_num;
};

static struct device_bus device_trees[DEVICE_BUS_MAX] = {
    [0 ... (DEVICE_BUS_MAX - 1)] = { RB_ROOT, 0 },
};

struct device_header *device__first_dev(enum device_bus_type bus_type)
{
	struct rb_node *node;

	if (bus_type >= DEVICE_BUS_MAX)
		return NULL;

	node = rb_first(&device_trees[bus_type].root);
	return node ? rb_entry(node, struct device_header, node) : NULL;
}

struct device_header *device__next_dev(struct device_header *dev) {
    struct rb_node *node = rb_next(&dev->node);
    return node ? rb_entry(node, struct device_header, node) : NULL;
}

// Reference: http://www.techedge.com.au/tech/8250tec.htm
struct serial8250_device {
    struct device_header	dev_hdr;
    pthread_mutex_t mutex;
    uint8_t			id;

    uint32_t			iobase;
    uint8_t			irq;
    uint8_t			irq_state;
    int			txcnt;
    int			rxcnt;
    int			rxdone;
    char		txbuf[64];
    char		rxbuf[64];

    uint8_t			dll; // Divisor Latch LOW
    uint8_t			dlm;
    uint8_t			iir;
    uint8_t			ier;
    uint8_t			fcr;
    uint8_t			lcr; // Line Control Reg
    uint8_t			mcr; // Modem Control Reg
    uint8_t			lsr; // Line Status Reg
    uint8_t			msr; // Modem Status Reg
    uint8_t			scr;
};

#define SERIAL_REGS_SETTING \
    .iir			= UART_IIR_NO_INT, \
    .lsr			= UART_LSR_TEMT | UART_LSR_THRE, \
    .msr			= UART_MSR_DCD | UART_MSR_DSR | UART_MSR_CTS, \
    .mcr			= UART_MCR_OUT2,

static struct serial8250_device devices[] = {
    /* ttyS0 */
    [0]	= {
        .dev_hdr = {
            .bus_type	= SERIAL8250_BUS_TYPE,
            .data		= NULL,
        },
        .mutex			= PTHREAD_MUTEX_INITIALIZER,

        .id			= 0,
        .iobase			= serial_iobase(0),
        .irq			= serial_irq(0),

        SERIAL_REGS_SETTING
    },
    /* ttyS1 */
    [1]	= {
        .dev_hdr = {
            .bus_type	= SERIAL8250_BUS_TYPE,
            .data		= NULL,
        },
        .mutex			= PTHREAD_MUTEX_INITIALIZER,

        .id			= 1,
        .iobase			= serial_iobase(1),
        .irq			= serial_irq(1),

        SERIAL_REGS_SETTING
    },
    /* ttyS2 */
    [2]	= {
        .dev_hdr = {
            .bus_type	= SERIAL8250_BUS_TYPE,
            .data		= NULL,
        },
        .mutex			= PTHREAD_MUTEX_INITIALIZER,

        .id			= 2,
        .iobase			= serial_iobase(2),
        .irq			= serial_irq(2),

        SERIAL_REGS_SETTING
    },
    /* ttyS3 */
    [3]	= {
        .dev_hdr = {
            .bus_type	= SERIAL8250_BUS_TYPE,
            .data		= NULL,
        },
        .mutex			= PTHREAD_MUTEX_INITIALIZER,

        .id			= 3,
        .iobase			= serial_iobase(3),
        .irq			= serial_irq(3),

        SERIAL_REGS_SETTING
    },
};

static void serial8250_flush_tx(struct kvm *kvm, struct serial8250_device *dev) {
    dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;

    if (dev->txcnt) {
        term_putc(dev->txbuf, dev->txcnt, dev->id);
        dev->txcnt = 0;
    }
}

static void serial8250_update_irq(struct kvm *kvm, struct serial8250_device *dev) {
    uint8_t iir = 0;

    if (dev->lcr & UART_FCR_CLEAR_RCVR) {
        dev->lcr &= ~UART_FCR_CLEAR_RCVR;
        dev->rxcnt = dev->rxdone = 0;
        dev->lsr &= ~UART_LSR_DR;
    }

    if (dev->lcr & UART_FCR_CLEAR_XMIT) {
        dev->lcr &= ~UART_FCR_CLEAR_XMIT;
        dev->txcnt = 0;
        dev->lsr |= UART_LSR_TEMT | UART_LSR_THRE;
    }

    if ((dev->ier & UART_IER_RDI) && (dev->lsr & UART_LSR_DR))
        iir |= UART_IIR_RDI;

    if ((dev->ier & UART_IER_THRI) && (dev->lsr & UART_LSR_TEMT))
        iir |= UART_IIR_THRI;

    if (!iir) {
        dev->iir = UART_IIR_NO_INT;
        if (dev->irq_state)
            kvm__irq_line(kvm, dev->irq, 0);
    } else {
        dev->iir = iir;
        if (!dev->irq_state)
            kvm__irq_line(kvm, dev->irq, 1);
    }
    dev->irq_state = iir;

    if (!(dev->ier & UART_IER_THRI))
        serial8250_flush_tx(kvm, dev);
}

static void serial8250_rx(struct serial8250_device *dev, void *data) {
    if (dev->rxdone == dev->rxcnt)
        return;

    /* Break issued ? */
    if (dev->lsr & UART_LSR_BI) {
        dev->lsr &= ~UART_LSR_BI;
        ioport__write8(data, 0);
        return;
    }

    ioport__write8(data, dev->rxbuf[dev->rxdone++]);
    if (dev->rxcnt == dev->rxdone) {
        dev->lsr &= ~UART_LSR_DR;
        dev->rxcnt = dev->rxdone = 0;
    }
}

void serial8250__sysrq(struct kvm *kvm, struct serial8250_device *dev) {
    dev->lsr |= UART_LSR_DR | UART_LSR_BI;
    dev->rxbuf[dev->rxcnt++] = sysrq_pending;
    sysrq_pending	= 0;
}


void serial8250__receive(struct kvm *kvm, struct serial8250_device *dev,
                int handle_sysrq) {
    int c;

    if (dev->mcr & UART_MCR_LOOP)
        return;

    if ((dev->lsr & UART_LSR_DR) || dev->rxcnt)
        return;

    if (handle_sysrq && sysrq_pending) {
        serial8250__sysrq(kvm, dev);
        return;
    }

    while (term_readable(dev->id) &&
           dev->rxcnt < 64) {

        c = term_getc(kvm, dev->id);

        if (c < 0)
            break;
        dev->rxbuf[dev->rxcnt++] = c;
        dev->lsr |= UART_LSR_DR;
    }
}

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))

void serial8250__update_consoles(struct kvm *kvm)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(devices); i++) {
		struct serial8250_device *dev = &devices[i];

		pthread_mutex_lock(&dev->mutex);

		/* Restrict sysrq injection to the first port */
		serial8250__receive(kvm, dev, i == 0);

		serial8250_update_irq(kvm, dev);

		pthread_mutex_unlock(&dev->mutex);
	}
}

static int serial8250_out(struct serial8250_device *dev, struct kvm_cpu *vcpu,
               uint16_t offset, void *data) {
    int ret = 1;
    char *addr = data;
    // printf("data is: %p\n", data);

    pthread_mutex_lock(&dev->mutex);

    // printf("serial8250_out offset: %d\n", offset);
    switch (offset) {
    case UART_TX:
        if (dev->lcr & UART_LCR_DLAB) {
            dev->dll = ioport__read8(data);
            break;
        }

        if (dev->mcr & UART_MCR_LOOP) {
            if (dev->rxcnt < 64) {
                dev->rxbuf[dev->rxcnt++] = *addr;
                dev->lsr |= UART_LSR_DR;
            }
            break;
        }

        if (dev->txcnt < 64) {
            dev->txbuf[dev->txcnt++] = *addr;
            dev->lsr &= ~UART_LSR_TEMT;
            serial8250_flush_tx(vcpu->kvm, dev);
        } 
        break;
    case UART_IER:
        if (!(dev->lcr & UART_LCR_DLAB))
            dev->ier = ioport__read8(data) & 0x0f;
        else
            dev->dlm = ioport__read8(data);
        break;
    case UART_FCR:
        dev->fcr = ioport__read8(data);
        break;
    case UART_LCR:
        dev->lcr = ioport__read8(data);
        break;
    case UART_MCR:
        dev->mcr = ioport__read8(data);
        break;
    case UART_SCR:
        dev->scr = ioport__read8(data);
        break;
    default:
        ret = 0;
        break;
    }

    serial8250_update_irq(vcpu->kvm, dev);

    pthread_mutex_unlock(&dev->mutex);


    return ret;
}

static int serial8250_in(struct serial8250_device *dev, struct kvm_cpu *vcpu,
              uint16_t offset, void *data) {
    int ret = 1;

    pthread_mutex_lock(&dev->mutex);

    switch (offset) {
    case UART_RX:
        if (dev->lcr & UART_LCR_DLAB)
            ioport__write8(data, dev->dll);
        else
            serial8250_rx(dev, data);
        break;
    case UART_IER:
        if (dev->lcr & UART_LCR_DLAB)
            ioport__write8(data, dev->dlm);
        else
            ioport__write8(data, dev->ier);
        break;
    case UART_IIR:
        ioport__write8(data, dev->iir | 0xc0);
        break;
    case UART_LCR:
        ioport__write8(data, dev->lcr);
        break;
    case UART_MCR:
        ioport__write8(data, dev->mcr);
        break;
    case UART_LSR:
        ioport__write8(data, dev->lsr);
        break;
    case UART_MSR:
        ioport__write8(data, dev->msr);
        break;
    case UART_SCR:
        ioport__write8(data, dev->scr);
        break;
    default:
        ret = 0;
        break;
    }

    serial8250_update_irq(vcpu->kvm, dev);

    pthread_mutex_unlock(&dev->mutex);

    return ret;
}

static void serial8250_mmio(struct kvm_cpu *vcpu, uint64_t addr, uint8_t *data, uint32_t len,
                uint8_t is_write, void *ptr) {
    struct serial8250_device *dev = ptr;

    // printf("is write: %d\n", is_write);
    // printf("ptr address: %p\n", ptr);

    if (is_write)
        serial8250_out(dev, vcpu, addr - dev->iobase, data);
    else
        serial8250_in(dev, vcpu, addr - dev->iobase, data);
}

int serial8250__init(struct kvm *kvm) {
    unsigned int i;
    int r = 0;

    for (i = 0; i < 4; i++) {
        struct serial8250_device *dev = &devices[i];

        r = kvm__register_iotrap(kvm, dev->iobase, 8, serial8250_mmio, dev,
                 SERIAL8250_BUS_TYPE);
        if (r < 0)
            break;
    }

    return r;
}
