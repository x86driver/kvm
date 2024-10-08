#ifndef _I8042_H_
#define _I8042_H_

#include <stdint.h>
#include "kvm.h"

struct kbd_state {
    struct kvm		*kvm;

    uint8_t			kq[128];	/* Keyboard queue */
    int			kread, kwrite;	/* Indexes into the queue */
    int			kcount;		/* number of elements in queue */

    uint8_t			mq[128];
    int			mread, mwrite;
    int			mcount;

    uint8_t			mstatus;	/* Mouse status byte */
    uint8_t			mres;		/* Current mouse resolution */
    uint8_t			msample;	/* Current mouse samples/second */

    uint8_t			mode;		/* i8042 mode register */
    uint8_t			status;		/* i8042 status register */
    /*
     * Some commands (on port 0x64) have arguments;
     * we store the command here while we wait for the argument
     */
    uint8_t			write_cmd;
};

static struct kbd_state		state;

static inline uint8_t ioport__read8(uint8_t *data)
{
    return *data;
}

static inline void ioport__write8(uint8_t *data, uint8_t value)
{
    *data		 = value;
}

static void kbd_update_irq(void)
{
    uint8_t klevel = 0;
    uint8_t mlevel = 0;

    /* First, clear the kbd and aux output buffer full bits */
    state.status &= ~(0x01 | 0x20);

    if (state.kcount > 0) {
        state.status |= 0x01;
        klevel = 1;
    }

    /* Keyboard has higher priority than mouse */
    if (klevel == 0 && state.mcount != 0) {
        state.status |= 0x21;
        mlevel = 1;
    }

    kvm__irq_line(state.kvm, 1, klevel);
    kvm__irq_line(state.kvm, 12, mlevel);
}

void mouse_queue(uint8_t c) {
    if (state.mcount >= 128)
        return;

    state.mq[state.mwrite++ % 128] = c;

    state.mcount++;
    kbd_update_irq();
}

void kbd_queue(uint8_t c) {
    if (state.kcount >= 128)
        return;

    state.kq[state.kwrite++ % 128] = c;

    state.kcount++;
    kbd_update_irq();
}

static void kbd_write_command(struct kvm *kvm, uint8_t val)
{
    switch (val) {
    case 0x20:
        if (state.kcount >= 128)
            break;

        state.kq[state.kwrite++ % 128] = state.mode;

        state.kcount++;
        kbd_update_irq();

        break;
    case 0x60:
    case 0xD4:
    case 0xD3:
        state.write_cmd = val;
        break;
    case 0xA9:
        /* 0 means we're a normal PS/2 mouse */
        mouse_queue(0);
        break;
    case 0xA7:
        state.mode |= 0x20;
        break;
    case 0xA8:
        state.mode &= ~0x20;
        break;
    case 0xFE:
        if (!kvm->cpus[0] || kvm->cpus[0]->thread == 0)
            break;
        
        pthread_kill(kvm->cpus[0]->thread, SIGRTMIN);
        break;
    default:
        break;
    }
}

static void kbd_write_data(uint8_t val)
{
    switch (state.write_cmd) {
    case 0x60:
        state.mode = val;
        kbd_update_irq();
        break;
    case 0xD3:
        mouse_queue(val);
        mouse_queue(0xFA);
        break;
    case 0xD4:
        /* The OS wants to send a command to the mouse */
        mouse_queue(0xFA);
        switch (val) {
        case 0xe6:
            /* set scaling = 1:1 */
            state.mstatus &= ~0x10;
            break;
        case 0xe8:
            /* set resolution */
            state.mres = val;
            break;
        case 0xe9:
            /* Report mouse status/config */
            mouse_queue(state.mstatus);
            mouse_queue(state.mres);
            mouse_queue(state.msample);
            break;
        case 0xf2:
            /* send ID */
            mouse_queue(0); /* normal mouse */
            break;
        case 0xf3:
            /* set sample rate */
            state.msample = val;
            break;
        case 0xf4:
            /* enable reporting */
            state.mstatus |= 0x20;
            break;
        case 0xf5:
            state.mstatus &= ~0x20;
            break;
        case 0xf6:
            /* set defaults, just fall through to reset */
        case 0xff:
            /* reset */
            state.mstatus = 0x0;
            state.mres = 0x2;
            state.msample = 100;
            break;
        default:
            break;
        }
        break;
    case 0:
        /* Just send the ID */
        kbd_queue(0xFA);
        kbd_queue(0xab);
        kbd_queue(0x41);
        kbd_update_irq();
        break;
    default:
        /* Yeah whatever */
        break;
    }
    state.write_cmd = 0;
}

uint8_t kbd_read_data(void) {
    uint8_t ret;
    int i;

    if (state.kcount != 0) {
        /* Keyboard data gets read first */
        ret = state.kq[state.kread++ % 128];
        state.kcount--;
        kvm__irq_line(state.kvm, 1, 0);
        kbd_update_irq();
    } else if (state.mcount > 0) {
        /* Followed by the mouse */
        ret = state.mq[state.mread++ % 128];
        state.mcount--;
        kvm__irq_line(state.kvm, 12, 0);
        kbd_update_irq();
    } else {
        i = state.kread - 1;
        if (i < 0)
            i = 128;
        ret = state.kq[i];
    }
    return ret;
}
static void kbd_reset(void)
{
    state = (struct kbd_state) {
        .status		= 0x1c , 
        .mode		= 0x3,
        .mres		= 0x2,
        .msample	= 100,
    };
}

static void kbd_io(struct kvm_cpu *vcpu, uint64_t addr, uint8_t *data, uint32_t len,
           uint8_t is_write, void *ptr)
{
    uint8_t value = 0;

    if (is_write)
        value = ioport__read8(data);

    switch (addr) {
    case 0x64:
        if (is_write)
            kbd_write_command(vcpu->kvm, value);
        else
            value = state.status;
        break;
    case 0x60:
        if (is_write)
            kbd_write_data(value);
        else
            value = kbd_read_data();
        break;
    case 0x61:
        if (!is_write)
            value = 0x20;
        break;
    default:
        return;
    }

    if (!is_write)
        ioport__write8(data, value);
}

#endif
