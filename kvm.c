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

#define KVM_DEV "/dev/kvm"
#define MEMORY_SIZE (1 << 21)  // 2MB
#define VGA_START 0xB8000
#define BIOS_ENTRY 0xFFFF0
#define BOOT_SECTOR 0x7C00

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
    struct kvm_regs regs;

    ioctl(vcpu_fd, KVM_GET_SREGS, &sregs);
    ioctl(vcpu_fd, KVM_GET_REGS, &regs);

    // CS:IP = 0xF000:FFF0
    sregs.cs.base = 0xF0000;
    sregs.cs.selector = 0xF000;
    regs.rip = 0xFFF0;

    sregs.ds.base = sregs.es.base = sregs.fs.base = sregs.gs.base = sregs.ss.base = 0;
    sregs.ds.selector = sregs.es.selector = sregs.fs.selector = sregs.gs.selector = sregs.ss.selector = 0;

    ioctl(vcpu_fd, KVM_SET_SREGS, &sregs);
    ioctl(vcpu_fd, KVM_SET_REGS, &regs);
}

void load_bios() {
    uint8_t bios_code[] = {
        0xEA, 0x00, 0x7C, 0x00, 0x00  // JMP 0000:7C00
    };
    memcpy(guest_memory + BIOS_ENTRY, bios_code, sizeof(bios_code));
}

void load_boot_sector(const char *filename) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Failed to open boot sector file");
        exit(1);
    }

    size_t read = fread(guest_memory + BOOT_SECTOR, 1, 512, file);
    if (read != 512) {
        fprintf(stderr, "Failed to read full boot sector\n");
        exit(1);
    }

    fclose(file);
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

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s boot.bin\n", argv[0]);
        return 1;
    }

    setup_kvm();
    setup_cpu();
    load_bios();
    load_boot_sector(argv[1]);

    run_cpu();

    read_vga_from_host(11);

    munmap(run, ioctl(kvm_fd, KVM_GET_VCPU_MMAP_SIZE, NULL));
    munmap(guest_memory, MEMORY_SIZE);
    close(vcpu_fd);
    close(vm_fd);
    close(kvm_fd);

    return 0;
}
