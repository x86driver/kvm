#include <stdio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kvm.h>
#include <stdint.h>

int main() {
    struct kvm_sregs sregs;
    int ret;
    int kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvmfd == -1) {
        perror("open /dev/kvm");
        return 1;
    }
    int vmfd = ioctl(kvmfd, KVM_CREATE_VM, 0);
    if (vmfd == -1) {
        perror("KVM_CREATE_VM");
        return 1;
    }
    int version = ioctl(kvmfd, KVM_GET_API_VERSION, NULL);
    printf("KVM API version: %d\n", version);
    unsigned char *ram = (unsigned char*)mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    int kfd = open("test.bin", O_RDONLY);
    read(kfd, ram, 4096);
    close(kfd);
    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x0,
        .memory_size = 0x1000,
        .userspace_addr = (uint64_t)ram,
    };
    ret = ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region);
    if (ret == -1) {
        perror("KVM_SET_USER_MEMORY_REGION");
        return 1;
    }
    int vcpufd = ioctl(vmfd, KVM_CREATE_VCPU, 0);
    if (vcpufd == -1) {
        perror("KVM_CREATE_VCPU");
        return 1;
    }
    int mmap_size = ioctl(kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (mmap_size == -1) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        return 1;
    }
    struct kvm_run *run = (struct kvm_run*)mmap(NULL, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpufd, 0);
    ret = ioctl(vcpufd, KVM_GET_SREGS, &sregs);
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(vcpufd, KVM_SET_SREGS, &sregs);
    if (ret == -1) {
        perror("KVM_SET_SREGS");
        return 1;
    }
    struct kvm_regs regs = {
        .rip = 0,
        .rflags = 0x2,
    };
    ret = ioctl(vcpufd, KVM_SET_REGS, &regs);
    if (ret == -1) {
        perror("KVM_SET_REGS");
        return 1;
    }
    while (1) {
        ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret == -1) {
            perror("kvm_run");
            return 1;
        }
        switch (run->exit_reason) {
            case KVM_EXIT_HLT:
                puts("KVM_EXIT_HLT");
                return 0;
            case KVM_EXIT_IO:
                //if (run->io.direction == KVM_EXIT_IO_OUT && run->io.size == 1 && run->io.port == 0x3f8 && run->io.count == 1) {
                    putchar(*(((char*)run) + run->io.data_offset));
                //} else {
                //    puts("unhandled KVM_EXIT_IO");
                //    return -1;
                //}
                break;
            case KVM_EXIT_FAIL_ENTRY:
                puts("KVM_EXIT_FAIL_ENTRY");
                return -1;
            default:
                puts("unhandled exit");
                printf("exit_reason: %d\n", run->exit_reason);
                return -1;
        }
    }

    return 0;
}