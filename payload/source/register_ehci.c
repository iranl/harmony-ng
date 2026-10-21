#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/syscall.h>

#ifndef __NR_sethostname
#define __NR_sethostname (4000 + 148)
#endif

int main(int argc, char **argv) {
    int fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (fd < 0) {
        perror("open /dev/mem");
        return 1;
    }

    // Physical address of sys_call_table[148] (sys_sethostname)
    uint32_t slot_phys = 0x0000c990;
    uint32_t orig_fn = 0;

    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        read(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("read slot");
        close(fd);
        return 1;
    }

    printf("sys_call_table[148] at 0x%08x = 0x%08x (expected 0x8002fb4c)\n", slot_phys, orig_fn);
    if (orig_fn != 0x8002fb4c) {
        fprintf(stderr, "Slot mismatch! Aborting for safety.\n");
        close(fd);
        return 1;
    }

    // Swap slot to platform_device_register (0x801267cc)
    uint32_t new_fn = 0x801267cc;
    printf("Swapping slot to platform_device_register (0x%08x)...\n", new_fn);
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &new_fn, sizeof(new_fn)) != sizeof(new_fn)) {
        perror("write slot");
        close(fd);
        return 1;
    }

    // Call syscall(__NR_sethostname, 0x8023b808) -> platform_device_register(&ar7240_usb_device)
    printf("Invoking syscall with &ar7240_usb_device (0x8023b808)...\n");
    int res = syscall(__NR_sethostname, (void *)0x8023b808);
    int err = errno;

    printf("platform_device_register returned: %d, errno: %d (%s)\n", res, err, strerror(err));

    // Restore original pointer
    printf("Restoring original sys_call_table slot...\n");
    if (lseek(fd, slot_phys, SEEK_SET) < 0 ||
        write(fd, &orig_fn, sizeof(orig_fn)) != sizeof(orig_fn)) {
        perror("restore slot");
    }

    close(fd);
    printf("Done!\n");
    return 0;
}
