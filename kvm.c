#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>
#include <linux/kvm.h>
#include <sys/ioctl.h>

#define KVM_DEV "/dev/kvm"
#define MEMORY_SIZE (1 << 21)  // 2MB
#define VGA_START 0xB8000

int kvm_fd, vm_fd, vcpu_fd;
void *guest_memory;
struct kvm_run *run;

void setup_kvm() {
    kvm_fd = open(KVM_DEV, O_RDWR);
    if (kvm_fd < 0) {
        perror("Unable to open /dev/kvm");
        exit(1);
    }

    vm_fd = ioctl(kvm_fd, KVM_CREATE_VM, 0);
    if (vm_fd < 0) {
        perror("Unable to create VM");
        exit(1);
    }

    guest_memory = mmap(NULL, MEMORY_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (guest_memory == MAP_FAILED) {
        perror("Unable to allocate guest memory");
        exit(1);
    }

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0,
        .memory_size = 0xB8000,
        .userspace_addr = (unsigned long)guest_memory,
    };
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        exit(1);
    }

    region.slot = 1;
    region.guest_phys_addr = 0xC0000;
    region.memory_size = MEMORY_SIZE - 0xC0000;
    region.userspace_addr = (unsigned long)guest_memory + 0xC0000;
    if (ioctl(vm_fd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        exit(1);
    }

    vcpu_fd = ioctl(vm_fd, KVM_CREATE_VCPU, 0);
    if (vcpu_fd < 0) {
        perror("Unable to create VCPU");
        exit(1);
    }

    int mmap_size = ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        exit(1);
    }

    run = mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu_fd, 0);
    if (run == MAP_FAILED) {
        perror("mmap vcpu");
        exit(1);
    }
}

void setup_cpu() {
    struct kvm_sregs sregs;
    if (ioctl(vcpu_fd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        exit(1);
    }

    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    sregs.ds.base = 0;
    sregs.ds.selector = 0;
    sregs.es.base = 0;
    sregs.es.selector = 0;
    sregs.fs.base = 0;
    sregs.fs.selector = 0;
    sregs.gs.base = 0;
    sregs.gs.selector = 0;
    sregs.ss.base = 0;
    sregs.ss.selector = 0;

    if (ioctl(vcpu_fd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        exit(1);
    }

    struct kvm_regs regs = {
        .rip = 0,
        .rflags = 0x2,
    };

    if (ioctl(vcpu_fd, KVM_SET_REGS, &regs) < 0) {
        perror("KVM_SET_REGS");
        exit(1);
    }
}

void run_cpu() {
    while (1) {
        if (ioctl(vcpu_fd, KVM_RUN, NULL) < 0) {
            perror("KVM_RUN");
            exit(1);
        }

        switch (run->exit_reason) {
            case KVM_EXIT_HLT:
                printf("CPU halted\n");
                return;
            case KVM_EXIT_IO:
                if (run->io.direction == KVM_EXIT_IO_OUT && run->io.port == 0x3f8) {
                    putchar(*(((char *)run) + run->io.data_offset));
                }
                break;
            case KVM_EXIT_MMIO:
                printf("MMIO at 0x%llx\n", run->mmio.phys_addr);
                if (run->mmio.phys_addr >= 0xB8000 && run->mmio.phys_addr < 0xC0000) {
                    printf("VGA memory access at 0x%llx\n", run->mmio.phys_addr);
                    if (run->mmio.is_write) {
                        memcpy(guest_memory + run->mmio.phys_addr, run->mmio.data, run->mmio.len);
                    } else {
                        memcpy(run->mmio.data, guest_memory + run->mmio.phys_addr, run->mmio.len);
                    }
                }
                break;
            default:
                printf("Unhandled exit reason: %d\n", run->exit_reason);
                return;
        }
    }
}

void read_vga_from_host(int n) {
    char *vga_mem = guest_memory + VGA_START;
    printf("VGA Memory Content: ");
    for (int i = 0; i < n; i++) {
        printf("%c", vga_mem[i * 2]);
    }
    printf("\n");
}

int main() {
    setup_kvm();
    setup_cpu();

    unsigned char code[] = {
        0xb8, 0x00, 0xb8,       // mov ax, 0xb800
        0x8e, 0xd8,             // mov ds, ax
        0x8e, 0xc0,             // mov es, ax

        // Write "VM" to VGA
        0xc6, 0x06, 0x00, 0x00, 'V',  // mov byte [0x0000], 'V'
        0xc6, 0x06, 0x01, 0x00, 0x07, // mov byte [0x0001], 0x07
        0xc6, 0x06, 0x02, 0x00, 'M',  // mov byte [0x0002], 'M'
        0xc6, 0x06, 0x03, 0x00, 0x07, // mov byte [0x0003], 0x07

        // Write "HELLO" to VGA
        0xc6, 0x06, 0x04, 0x00, 'H',  // mov byte [0x0004], 'H'
        0xc6, 0x06, 0x05, 0x00, 0x07, // mov byte [0x0005], 0x07
        0xc6, 0x06, 0x06, 0x00, 'E',  // mov byte [0x0006], 'E'
        0xc6, 0x06, 0x07, 0x00, 0x07, // mov byte [0x0007], 0x07
        0xc6, 0x06, 0x08, 0x00, 'L',  // mov byte [0x0008], 'L'
        0xc6, 0x06, 0x09, 0x00, 0x07, // mov byte [0x0009], 0x07
        0xc6, 0x06, 0x0a, 0x00, 'L',  // mov byte [0x000a], 'L'
        0xc6, 0x06, 0x0b, 0x00, 0x07, // mov byte [0x000b], 0x07
        0xc6, 0x06, 0x0c, 0x00, 'O',  // mov byte [0x000c], 'O'
        0xc6, 0x06, 0x0d, 0x00, 0x07, // mov byte [0x000d], 0x07

        0xf4                    // hlt
    };

    memcpy(guest_memory, code, sizeof(code));

    run_cpu();

    read_vga_from_host(7);

    munmap(run, ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL));
    munmap(guest_memory, MEMORY_SIZE);
    close(vcpu_fd);
    close(vm_fd);
    close(kvm_fd);

    return 0;
}
