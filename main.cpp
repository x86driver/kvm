#include <stdio.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/kvm.h>
#include <stdint.h>
#include <termios.h>
#include <pthread.h>

#define RAM_SIZE 0x1000

unsigned char *ram;
struct kvm_run *run;
volatile int running = 1;

void* keyboard_thread(void* arg) {
    int vcpufd = *(int*)arg;
    char c;
    while (running) {
        c = getchar();
        if (c == 'q') {
            running = 0;
            break;
        }
        run->io.direction = KVM_EXIT_IO_IN;
        run->io.size = 1;
        run->io.port = 0xf1;
        run->io.count = 1;
        *((char *)((char *)run + run->io.data_offset)) = c;
        ioctl(vcpufd, KVM_RUN, NULL);
    }
    return NULL;
}

int setup_kvm(int *kvmfd, int *vmfd, int *vcpufd, int *mmap_size) {
    struct kvm_sregs sregs;
    int ret;

    *kvmfd = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (*kvmfd < 0) {
        perror("open /dev/kvm");
        return -1;
    }

    int version = ioctl(*kvmfd, KVM_GET_API_VERSION, NULL);
    if (version < 0) {
        perror("KVM_GET_API_VERSION");
        close(*kvmfd);
        return -1;
    }
    if (version != 12) {
        fprintf(stderr, "KVM API version is not 12\n");
        close(*kvmfd);
        return -1;
    }

    *vmfd = ioctl(*kvmfd, KVM_CREATE_VM, 0);
    if (*vmfd < 0) {
        perror("KVM_CREATE_VM");
        close(*kvmfd);
        return -1;
    }

    *vcpufd = ioctl(*vmfd, KVM_CREATE_VCPU, 0);
    if (*vcpufd < 0) {
        perror("KVM_CREATE_VCPU");
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }

    *mmap_size = ioctl(*kvmfd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (*mmap_size < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        close(*vcpufd);
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }

    run = (struct kvm_run*)mmap(NULL, *mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, *vcpufd, 0);
    if (run == MAP_FAILED) {
        perror("mmap kvm_run");
        close(*vcpufd);
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }

    ret = ioctl(*vcpufd, KVM_GET_SREGS, &sregs);
    if (ret < 0) {
        perror("KVM_GET_SREGS");
        munmap(run, *mmap_size);
        close(*vcpufd);
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }
    sregs.cs.base = 0;
    sregs.cs.selector = 0;
    ret = ioctl(*vcpufd, KVM_SET_SREGS, &sregs);
    if (ret < 0) {
        perror("KVM_SET_SREGS");
        munmap(run, *mmap_size);
        close(*vcpufd);
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }

    struct kvm_regs regs = {
        .rip = 0,
        .rflags = 0x2,
    };
    ret = ioctl(*vcpufd, KVM_SET_REGS, &regs);
    if (ret < 0) {
        perror("KVM_SET_REGS");
        munmap(run, *mmap_size);
        close(*vcpufd);
        close(*vmfd);
        close(*kvmfd);
        return -1;
    }

    return 0;
}

int load_binary_to_memory(const char *filename, unsigned char *memory, size_t size) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open input file");
        return -1;
    }
    ssize_t bytes_read = read(fd, memory, size);
    if (bytes_read < 0) {
        perror("read input file");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int main() {
    int kvmfd, vmfd, vcpufd, mmap_size;
    if (setup_kvm(&kvmfd, &vmfd, &vcpufd, &mmap_size) != 0) {
        return 1;
    }

    ram = (unsigned char*)mmap(NULL, RAM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (ram == MAP_FAILED) {
        perror("mmap");
        close(vcpufd);
        close(vmfd);
        close(kvmfd);
        return 1;
    }

    if (load_binary_to_memory("input.bin", ram, RAM_SIZE) != 0) {
        munmap(ram, RAM_SIZE);
        close(vcpufd);
        close(vmfd);
        close(kvmfd);
        return 1;
    }

    struct kvm_userspace_memory_region region = {
        .slot = 0,
        .guest_phys_addr = 0x0,
        .memory_size = RAM_SIZE,
        .userspace_addr = (uint64_t)ram,
    };
    if (ioctl(vmfd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        munmap(ram, RAM_SIZE);
        close(vcpufd);
        close(vmfd);
        close(kvmfd);
        return 1;
    }

    struct termios oldt, newt;
    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    pthread_t keyboard_tid;
    pthread_create(&keyboard_tid, NULL, keyboard_thread, &vcpufd);

    while (running) {
        int ret = ioctl(vcpufd, KVM_RUN, NULL);
        if (ret < 0) {
            perror("KVM_RUN");
            break;
        }

        switch (run->exit_reason) {
            case KVM_EXIT_HLT:
                puts("KVM_EXIT_HLT");
                running = 0;
                break;
            case KVM_EXIT_IO:
                if (run->io.direction == KVM_EXIT_IO_OUT && run->io.size == 1 && run->io.port == 0xf1 && run->io.count == 1) {
                    putchar(*(((char*)run) + run->io.data_offset));
                } else if (run->io.direction == KVM_EXIT_IO_IN && run->io.size == 1 && run->io.port == 0xf1 && run->io.count == 1) {
                    continue;
                } else {
                    puts("unhandled KVM_EXIT_IO");
                    running = 0;
                }
                break;
            case KVM_EXIT_FAIL_ENTRY:
                puts("KVM_EXIT_FAIL_ENTRY");
                running = 0;
                break;
            default:
                puts("unhandled exit");
                printf("exit_reason: %d\n", run->exit_reason);
                running = 0;
                break;
        }
    }

    pthread_join(keyboard_tid, NULL);
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    munmap(run, mmap_size);
    close(vcpufd);
    munmap(ram, RAM_SIZE);
    close(vmfd);
    close(kvmfd);

    return 0;
}
