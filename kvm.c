#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <asm/bootparam.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <limits.h>
#include "kvm.h"
#include "rbtree.h"
#include "mmio.h"
#include "i8042.h"
#include "devices.h"
#include "term.h"
#include "mptable.h"

#define KVM_DEV "/dev/kvm"

void serial8250__update_consoles(struct kvm *kvm);
int serial8250__init(struct kvm *kvm);
static pthread_mutex_t mmio_lock;
static char kern_cmdline[2048] = "noapic noacpi pci=conf1 reboot=k panic=1 i8042.direct=1 i8042.dumbkbd=1 i8042.nopnp=1 earlyprintk=serial i8042.noaux=1 console=ttyS0 root=/dev/vda rw ";
static const char *BZIMAGE_MAGIC = "HdrS";
static struct rb_root pio_tree = RB_ROOT;

struct kvm_cpu *kvm_cpu__arch_init(struct kvm *kvm, unsigned long cpu_id) {

    struct kvm_cpu *vcpu = malloc(sizeof(struct kvm_cpu));
    int mmap_size;

    vcpu->kvm = kvm;

    if (!vcpu)
        return NULL;

    vcpu->cpu_id = cpu_id;
    vcpu->vcpu_fd = ioctl(vcpu->kvm->vm_fd, KVM_CREATE_VCPU, cpu_id);
    if (vcpu->vcpu_fd < 0)
        perror("KVM_CREATE_VCPU ioctl");

    mmap_size = ioctl(vcpu->kvm->sys_fd, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (mmap_size < 0)
        perror("KVM_GET_VCPU_MMAP_SIZE ioctl");

    vcpu->kvm_run = mmap(NULL, mmap_size, PROT_READ|PROT_WRITE, MAP_SHARED, vcpu->vcpu_fd, 0);
    if (vcpu->kvm_run == MAP_FAILED)
        perror("unable to mmap vcpu fd");


    return vcpu;
}

int kvm_cpu__init(struct kvm *kvm) {

    // Set number of CPUS
    kvm->cpus = calloc(kvm->nrcpus + 1, sizeof(void *));

    if (!kvm->cpus)
    {
        printf("Couldn't allocate array for %d CPUs\n", kvm->nrcpus);
        return -1;
    }

    for (int i = 0; i < kvm->nrcpus; i++)
    {
        kvm->cpus[i] = kvm_cpu__arch_init(kvm, i);
        if (!kvm->cpus[i])
        {
            printf("unable to initialize KVM VCPU\n");
            goto fail_alloc;
        }
    }

    return 0;

fail_alloc:
    for (int i = 0; i < kvm->nrcpus; i++)
        free(kvm->cpus[i]);

    return -1;
}

void filter_cpuid(struct kvm_cpuid2 *kvm_cpuid, int cpu_id) {
    unsigned int i;

    for (i = 0; i < kvm_cpuid->nent; i++) {
        struct kvm_cpuid_entry2 *entry = &kvm_cpuid->entries[i];

        switch (entry->function) {
        case 1:
            entry->ebx &= ~(0xff << 24);
            entry->ebx |= cpu_id << 24;
            /* Set X86_FEATURE_HYPERVISOR */
            if (entry->index == 0)
                entry->ecx |= (1 << 31);
            break;
        case 6:
            entry->ecx = entry->ecx & ~(1 << 3);
            break;
        case 10: { /* Architectural Performance Monitoring */
            union cpuid10_eax {
                struct {
                    unsigned int version_id		:8;
                    unsigned int num_counters	:8;
                    unsigned int bit_width		:8;
                    unsigned int mask_length	:8;
                } split;
                unsigned int full;
            } eax;

            if (entry->eax) {
                eax.full = entry->eax;
                if (eax.split.version_id != 2 ||
                    !eax.split.num_counters)
                    entry->eax = 0;
            }
            break;
        }
        default:
            break;
        };
    }
}

void kvm_cpu__setup_cpuid(struct kvm_cpu *vcpu) {
    struct kvm_cpuid2 *kvm_cpuid;

    kvm_cpuid = calloc(1, sizeof(*kvm_cpuid) +
                100 * sizeof(*kvm_cpuid->entries));

    kvm_cpuid->nent = 100;
    if (ioctl(vcpu->kvm->sys_fd, KVM_GET_SUPPORTED_CPUID, kvm_cpuid) < 0)
        perror("KVM_GET_SUPPORTED_CPUID failed");

    filter_cpuid(kvm_cpuid, vcpu->cpu_id);

    if (ioctl(vcpu->vcpu_fd, KVM_SET_CPUID2, kvm_cpuid) < 0)
        perror("KVM_SET_CPUID2 failed");

    free(kvm_cpuid);
}

static inline uint32_t selector_to_base(uint16_t selector) {
    return (uint32_t)selector << 4;
}

static void kvm_cpu__setup_sregs(struct kvm_cpu *vcpu) {
    if (ioctl(vcpu->vcpu_fd, KVM_GET_SREGS, &vcpu->sregs) < 0)
        perror("KVM_GET_SREGS failed");

    vcpu->sregs.cs.selector	= 0x1000;
    vcpu->sregs.cs.base	= selector_to_base(0x1000);
    vcpu->sregs.ss.selector	= 0x1000;
    vcpu->sregs.ss.base	= selector_to_base(0x1000);
    vcpu->sregs.ds.selector	= 0x1000;
    vcpu->sregs.ds.base	= selector_to_base(0x1000);
    vcpu->sregs.es.selector	= 0x1000;
    vcpu->sregs.es.base	= selector_to_base(0x1000);
    vcpu->sregs.fs.selector	= 0x1000;
    vcpu->sregs.fs.base	= selector_to_base(0x1000);
    vcpu->sregs.gs.selector	= 0x1000;
    vcpu->sregs.gs.base	= selector_to_base(0x1000);

    if (ioctl(vcpu->vcpu_fd, KVM_SET_SREGS, &vcpu->sregs) < 0)
        perror("KVM_SET_SREGS failed");
}

static void kvm_cpu__setup_regs(struct kvm_cpu *vcpu) {
    vcpu->regs = (struct kvm_regs) {
        /* We start the guest in 16-bit real mode  */
        .rflags	= 0x0000000000000002ULL,

        .rip	= 0x200,
        .rsp	= 0x8000,
        .rbp	= 0x8000,
    };

    if (vcpu->regs.rip > USHRT_MAX)
        printf("ip 0x%lx is too high for real mode\n", (uint64_t)vcpu->regs.rip);

    if (ioctl(vcpu->vcpu_fd, KVM_SET_REGS, &vcpu->regs) < 0)
        perror("KVM_SET_REGS failed");
}

void kvm_cpu__reset_vcpu(struct kvm_cpu *vcpu) {
    kvm_cpu__setup_cpuid(vcpu);
    kvm_cpu__setup_sregs(vcpu);
    kvm_cpu__setup_regs(vcpu);
}

void kvm__irq_line(struct kvm *kvm, int irq, int level)
{
    struct kvm_irq_level irq_level;

    irq_level = (struct kvm_irq_level){
        {
            .irq = irq,
        },
        .level = level,
    };

    if (ioctl(kvm->vm_fd, KVM_IRQ_LINE, &irq_level) < 0)
        perror("KVM_IRQ_LINE failed");
}

struct rb_int_node *rb_int_search_single(struct rb_root *root, uint64_t point) {
    struct rb_node *node = root->rb_node;

    while (node) {
        struct rb_int_node *cur = rb_int(node);

        if (point < cur->low)
            node = node->rb_left;
        else if (cur->high <= point)
            node = node->rb_right;
        else
            return cur;
    }

    return NULL;
}

struct rb_int_node *rb_int_search_range(struct rb_root *root, uint64_t low, uint64_t high) {
    struct rb_int_node *range;

    range = rb_int_search_single(root, low);
    if (range == NULL)
        return NULL;

    if (range->high < high)
        return NULL;

    return range;
}

int rb_int_insert(struct rb_root *root, struct rb_int_node *i_node) {
    struct rb_node **node = &root->rb_node, *parent = NULL;

    while (*node) {
        struct rb_int_node *cur = rb_int(*node);

        parent = *node;
        if (i_node->high <= cur->low)
            node = &cur->node.rb_left;
        else if (cur->high <= i_node->low)
            node = &cur->node.rb_right;
        else
            return -EEXIST;
    }

    rb_link_node(&i_node->node, parent, node);
    rb_insert_color(&i_node->node, root);

    return 0;
}

static struct mmio_mapping *mmio_search(struct rb_root *root, uint64_t addr, uint64_t len) {
    struct rb_int_node *node;

    if (addr + len <= addr)
        return NULL;

    node = rb_int_search_range(root, addr, addr + len);
    if (node == NULL)
        return NULL;

    return mmio_node(node);
}

static struct mmio_mapping *mmio_search_single(struct rb_root *root, uint64_t addr) {
    struct rb_int_node *node;

    node = rb_int_search_single(root, addr);
    if (node == NULL)
        return NULL;

    return mmio_node(node);
}

static void mmio_remove(struct rb_root *root, struct mmio_mapping *data) {
    rb_erase(&data->node.node, root);
}

static void mmio_deregister(struct kvm *kvm, struct rb_root *root, struct mmio_mapping *mmio) {
    struct kvm_coalesced_mmio_zone zone = (struct kvm_coalesced_mmio_zone) {
        .addr	= rb_int_start(&mmio->node),
        .size	= 1,
    };
    ioctl(kvm->vm_fd, KVM_UNREGISTER_COALESCED_MMIO, &zone);

    mmio_remove(root, mmio);
    free(mmio);
}

static int mmio_insert(struct rb_root *root, struct mmio_mapping *data) {
    return rb_int_insert(root, &data->node);
}

static struct mmio_mapping *mmio_get(struct rb_root *root, uint64_t phys_addr, uint32_t len) {
    struct mmio_mapping *mmio;

    pthread_mutex_lock(&mmio_lock);
    mmio = mmio_search(root, phys_addr, len);
    if (mmio)
        mmio->refcount++;
    pthread_mutex_unlock(&mmio_lock);

    return mmio;
}

static void mmio_put(struct kvm *kvm, struct rb_root *root, struct mmio_mapping *mmio)
{
    pthread_mutex_lock(&mmio_lock);
    mmio->refcount--;
    if (mmio->remove && mmio->refcount == 0)
        mmio_deregister(kvm, root, mmio);
    pthread_mutex_unlock(&mmio_lock);
}

int kvm__register_iotrap(struct kvm *kvm, uint64_t phys_addr, uint64_t phys_addr_len,
             mmio_handler_fn mmio_fn, void *ptr,
             unsigned int flags) {
    struct mmio_mapping *mmio;
    int ret;

    mmio = malloc(sizeof(*mmio));
    if (mmio == NULL)
        return -ENOMEM;

    *mmio = (struct mmio_mapping) {
        .node		= RB_INT_INIT(phys_addr, phys_addr + phys_addr_len),
        .mmio_fn	= mmio_fn,
        .ptr		= ptr,
        /*
         * Start from 0 because kvm__deregister_mmio() doesn't decrement
         * the reference count.
         */
        .refcount	= 0,
        .remove		= 0,
    };

    pthread_mutex_lock(&mmio_lock);
    ret = mmio_insert(&pio_tree, mmio);
    pthread_mutex_unlock(&mmio_lock);

    return ret;
}

int kvm__deregister_iotrap(struct kvm *kvm, uint64_t phys_addr, unsigned int flags) {
    struct mmio_mapping *mmio;
    struct rb_root *tree;

    tree = &pio_tree;

    pthread_mutex_lock(&mmio_lock);
    mmio = mmio_search_single(tree, phys_addr);
    if (mmio == NULL) {
        pthread_mutex_unlock(&mmio_lock);
        return 0;
    }

    if (mmio->refcount == 0)
        mmio_deregister(kvm, tree, mmio);
    else
        mmio->remove = 1;
    pthread_mutex_unlock(&mmio_lock);

    return 1;
}

void kvm__arch_read_term(struct kvm *kvm) {
    serial8250__update_consoles(kvm);
}

void setup_kvm(struct kvm *kvm) {
    int ret = 0;

    kvm->sys_fd = open(KVM_DEV, O_RDONLY);
    if (kvm->sys_fd < 0) {
        perror("Unable to open /dev/kvm");
        exit(1);
    }

    ret = ioctl(kvm->sys_fd, KVM_GET_API_VERSION, NULL);
    if (ret < 0 || ret != KVM_API_VERSION) {
        perror("KVM_GET_API_VERSION");
        exit(1);
    }

    kvm->vm_fd = ioctl(kvm->sys_fd, KVM_CREATE_VM, 0);
    if (kvm->vm_fd < 0) {
        perror("Unable to create VM");
        exit(1);
    }
}

ssize_t xread(int fd, void *buf, size_t count) {
    ssize_t nr;

restart:
    nr = read(fd, buf, count);
    if ((nr < 0) && ((errno == EAGAIN) || (errno == EINTR)))
        goto restart;

    return nr;
}

ssize_t read_in_full(int fd, void *buf, size_t count) {
    ssize_t total = 0;
    char *p = buf;

    while (count > 0)
    {
        ssize_t nr;

        nr = xread(fd, p, count);
        if (nr <= 0)
        {
            if (total > 0)
                return total;

            return -1;
        }

        count -= nr;
        total += nr;
        p += nr;
    }

    return total;
}

ssize_t read_file(int fd, char *buf, size_t max_size) {
    ssize_t ret;
    char dummy;

    errno = 0;
    ret = read_in_full(fd, buf, max_size);

    /* Probe whether we reached EOF. */
    if (xread(fd, &dummy, 1) == 0)
        return ret;

    errno = ENOMEM;
    return -1;
}

void kvm__arch_init(struct kvm *kvm) {

    kvm->ram_slots = 0;

    kvm->ram_size = RAM_SIZE;

    struct kvm_pit_config pit_config = {
        .flags = 0,
    };
    int ret;
    ret = ioctl(kvm->vm_fd, KVM_SET_TSS_ADDR, 0xfffbd000);
    if (ret < 0)
        perror("KVM_SET_TSS_ADDR ioctl");

    ret = ioctl(kvm->vm_fd, KVM_CREATE_PIT2, &pit_config);
    if (ret < 0)
        perror("KVM_CREATE_PIT2 ioctl");

    kvm->ram_start = mmap(NULL, kvm->ram_size,
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

    if ((unsigned)kvm->ram_size >= (unsigned)KVM_32BIT_GAP_START) {
        kvm->ram_size = kvm->ram_size + KVM_32BIT_GAP_SIZE;
        // if ram size is bigger than the 32bit RAM, then mprotect the gap PROT_NONE
        // so that we will konw if the programme accidently writes to this address
        if (kvm->ram_start != MAP_FAILED)
            mprotect(kvm->ram_start + KVM_32BIT_GAP_START, KVM_32BIT_GAP_START, PROT_NONE);
    }

    if (kvm->ram_start == MAP_FAILED)
        perror("out of memory");

    // Create virtual interrupt chip
    ret = ioctl(kvm->vm_fd, KVM_CREATE_IRQCHIP);
    if (ret < 0)
        perror("KVM_CREATE_IRQCHIP ioctl");
}

void kvm_ram__init(struct kvm *kvm) {
    struct kvm_userspace_memory_region mem;
    struct kvm_mem_bank *bank;
    struct list_head *prev_entry;
    int ret = 0;

    kvm__arch_init(kvm);

    INIT_LIST_HEAD(&kvm->mem_banks);

    if (pthread_mutex_lock(&kvm->mutex) != 0)
        perror("pthread_mtex_lock");

    prev_entry = &kvm->mem_banks;

    bank = malloc(sizeof(struct kvm_mem_bank));
    if (!bank) {
        perror("malloc");
        exit(1);
    }

    INIT_LIST_HEAD(&bank->list);
    bank->guest_phys_addr = 0;
    bank->host_addr = kvm->ram_start;
    bank->size = kvm->ram_size;
    bank->slot = 0;

    if (kvm->ram_size < KVM_32BIT_GAP_START) {
        mem = (struct kvm_userspace_memory_region) {
            .slot = 0,
            .flags = 0,
            .guest_phys_addr = 0,
            .memory_size = kvm->ram_size,
            .userspace_addr = (unsigned long)kvm->ram_start,
        };

        ret = ioctl(kvm->vm_fd, KVM_SET_USER_MEMORY_REGION, &mem);
        if (ret < 0) {
            perror("KVM_SET_USER_MEMORY_REGION ioctl");
            exit(1);
        }

        list_add(&bank->list, prev_entry);
    }
    if (pthread_mutex_unlock(&kvm->mutex) != 0)
        perror("pthread_mutex_unlock");
}

void *guest_flat_to_host(struct kvm *kvm, uint64_t offset) {
    struct kvm_mem_bank *bank;

    list_for_each_entry(bank, &kvm->mem_banks, list)
    {
        uint64_t bank_start = bank->guest_phys_addr;
        uint64_t bank_end = bank_start + bank->size;

        if (offset >= bank_start && offset < bank_end)
            return bank->host_addr + (offset - bank_start);
    }

    printf("unable to translate guest address 0x%llx to host\n",
           (unsigned long long)offset);
    return NULL;
}

static inline void *guest_real_to_host(struct kvm *kvm, uint16_t selector, uint16_t offset) {
    unsigned long flat = ((uint32_t)selector << 4) + offset;

    return guest_flat_to_host(kvm, flat);
}

int kvm__load_kernel(struct kvm *kvm) {

    int ret = 0;
    int fd_kernel = -1, fd_initrd = -1;

    struct boot_params *kern_boot;
    struct boot_params boot;
    size_t cmdline_size;
    ssize_t file_size;
    void *p;

    fd_kernel = open(kvm->kernel_filename, O_RDONLY);
    if (fd_kernel < 0) {
        printf("Unable to open kernel %s\n", kvm->kernel_filename);
        return -1;
    }

    fd_initrd = open(kvm->initrd_filename, O_RDONLY);
    if (fd_initrd < 0) {
        printf("Unable to open initrd %s\n", kvm->initrd_filename);
        return -1;
    }

    if (read_in_full(fd_kernel, &boot, sizeof(boot)) != sizeof(boot))
        return -1;

    if (memcmp(&boot.hdr.header, BZIMAGE_MAGIC, strlen(BZIMAGE_MAGIC)))
        return -1;

    if (lseek(fd_kernel, 0, SEEK_SET) < 0)
        perror("lseek");

    if (!boot.hdr.setup_sects)
        boot.hdr.setup_sects = 4;
    file_size = (boot.hdr.setup_sects + 1) << 9;
    p = guest_real_to_host(kvm, 0x1000, 0x00);
    if (read_in_full(fd_kernel, p, file_size) != file_size)
        perror("kernel setup read");

    p = guest_flat_to_host(kvm, 0x100000UL);
    file_size = read_file(fd_kernel, p, kvm->ram_size - 0x100000UL);

    if (file_size < 0)
        perror("kernel read");

    // copy cmdline to host
    p = guest_flat_to_host(kvm, 0x20000);
    cmdline_size = strlen(kern_cmdline) + 1;
    if (cmdline_size > boot.hdr.cmdline_size)
        cmdline_size = boot.hdr.cmdline_size;

    memset(p, 0, boot.hdr.cmdline_size);
    memcpy(p, kern_cmdline, cmdline_size - 1);

    kern_boot = guest_real_to_host(kvm, 0x1000, 0x00);

    kern_boot->hdr.cmd_line_ptr = 0x20000;
    kern_boot->hdr.type_of_loader = 0xff;
    kern_boot->hdr.heap_end_ptr = 0xfe00;
    kern_boot->hdr.loadflags |= CAN_USE_HEAP;
    kern_boot->hdr.vid_mode = 0;

    // read initrd image into guest memory
    struct stat initrd_stat;
    unsigned long addr;

    if (fstat(fd_initrd, &initrd_stat))
        perror("fstat");

    addr = boot.hdr.initrd_addr_max & ~0xfffff;
    for (;;)
    {
        if (addr < 0x100000UL)
        {
            printf("Not enough memory for initrd\n");
            return -1;
        }
        else if (addr < (kvm->ram_size - initrd_stat.st_size))
            break;

        addr -= 0x100000;
    }

    p = guest_flat_to_host(kvm, addr);
    if (read_in_full(fd_initrd, p, initrd_stat.st_size) < 0)
        perror("Failed to read initrd");

    kern_boot->hdr.ramdisk_image = addr;
    kern_boot->hdr.ramdisk_size = initrd_stat.st_size;

    close(fd_initrd);
    close(fd_kernel);

    return ret;
}

int kbd__init(struct kvm *kvm)
{
    int r;

    kbd_reset();
    state.kvm = kvm;
    r = kvm__register_iotrap(kvm, 0x60, 2, kbd_io, NULL, DEVICE_BUS_IOPORT);
    if (r < 0)
        return r;
    r = kvm__register_iotrap(kvm, 0x64, 2, kbd_io, NULL, DEVICE_BUS_IOPORT);
    if (r < 0) {
        kvm__deregister_iotrap(kvm, 0x60, DEVICE_BUS_IOPORT);
        return r;
    }

    return 0;
}

static inline int kvm_cpu__emulate_io(struct kvm_cpu *vcpu, uint16_t port, void *data,
                        int direction, int size, uint32_t count) {

    struct mmio_mapping *mmio;
    int is_write;

    if (direction == 1)
        is_write = 1;
    else
        is_write = 0;


    mmio = mmio_get(&pio_tree, port, size);
    if (!mmio) {
        return 1;
    }

    while (count--) {
        mmio->mmio_fn(vcpu, port, data, size, is_write, mmio->ptr);

        data += size;
    }
    mmio_put(vcpu->kvm, &pio_tree, mmio);

    return 1;
}

void *kvm_cpu__start(void *_cpu) {
    int err = 0;

    struct kvm_cpu *cpu = _cpu;
    kvm_cpu__reset_vcpu(cpu);

    // always run the kvm
    while (1) {
        err = ioctl(cpu->vcpu_fd, KVM_RUN, 0);
        if (err < 0 && (errno != EINTR && errno != EAGAIN))
            perror("KVM_RUN ioctl");

        // printf("switch kvm run exit reason: %d\n", cpu->kvm_run->exit_reason);
        switch (cpu->kvm_run->exit_reason) {
        case KVM_EXIT_UNKNOWN:
            break;
        case KVM_EXIT_IO: {
            int ret;

            ret = kvm_cpu__emulate_io(cpu,
                          cpu->kvm_run->io.port,
                          (uint8_t *)cpu->kvm_run +
                          cpu->kvm_run->io.data_offset,
                          cpu->kvm_run->io.direction,
                          cpu->kvm_run->io.size,
                          cpu->kvm_run->io.count);
            if (!ret) {
                err = 1;
                goto panic_kvm;
            }

            break;
        }

        default: {
            goto panic_kvm;

            break;
        }

        }
    }

panic_kvm:
    return NULL;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s bzImage initrd\n", argv[0]);
        return 1;
    }

    struct kvm *kvm = calloc(sizeof(struct kvm), 1);
    kvm->kernel_filename = argv[1];
    kvm->initrd_filename = argv[2];
    kvm->nrcpus = 32;

    setup_kvm(kvm);
    kvm_ram__init(kvm);

    if (kvm__load_kernel(kvm) < 0) {
        fprintf(stderr, "Failed to load kernel\n");
        return 1;
    }

    kvm__setup_bios(kvm);

    if (mptable__init(kvm) < 0) {
        fprintf(stderr, "Failed to initialize MP table\n");
        return 1;
    }

    if (kvm_cpu__init(kvm) < 0) {
        fprintf(stderr, "Failed to initialize CPU\n");
        return 1;
    }

    if (serial8250__init(kvm) < 0) {
        fprintf(stderr, "Failed to initialize serial port\n");
        return 1;
    }

    if (term_init(kvm) < 0) {
        fprintf(stderr, "Failed to initialize terminal\n");
        return 1;
    }

    if (kbd__init(kvm) < 0) {
        fprintf(stderr, "Failed to initialize keyboard\n");
        return 1;
    }

    // start the kvm
    for (int i = 0; i < kvm->nrcpus; i++)
    {
        if (pthread_create(&kvm->cpus[i]->thread, NULL, kvm_cpu__start, kvm->cpus[i]) != 0)
            perror("unable to create KVM VCPU thread");
    }

    if (pthread_join(kvm->cpus[0]->thread, NULL) != 0)
        perror("unable to join with vcpu 0");

    // do not need to pause kvm, kill the thread directly
    for (int i = 0; i < kvm->nrcpus; i++)
    {
        pthread_kill(kvm->cpus[i]->thread, SIGRTMIN);
    }

    free(kvm->cpus[0]);
    kvm->cpus[0] = NULL;

    free(kvm->cpus);
    free(kvm);

    return 0;
}
