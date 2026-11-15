/* Runs a program the way it runs on the Hub, whose kernel has no futex system call, so that no
 * futex wait ever sleeps there: a seccomp filter answers every futex WAIT with EAGAIN (glibc
 * treats that as "try again"; it aborts on ENOSYS, which uClibc on the Hub ignores), wakes go
 * through, then the program is started.
 *   no_futex_exec <program> [args...]
 * Waits built on futex (pthread_cond_wait, sem_wait, pthread_join, a contended mutex) then
 * return at once, as on the Hub, so a thread waiting on one spins instead of sleeping. */
#include <errno.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/futex.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
        return 2;
    }
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_futex, 0, 5),
        /* the operation without the private and clock flags (low word of the second argument) */
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[1])),
        BPF_STMT(BPF_ALU | BPF_AND | BPF_K, FUTEX_CMD_MASK),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FUTEX_WAIT, 1, 0),
        BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, FUTEX_WAIT_BITSET, 0, 1),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | (EAGAIN & SECCOMP_RET_DATA)),
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog = { (unsigned short)(sizeof(filter) / sizeof(filter[0])), filter };
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) != 0) {
        perror("seccomp");
        return 2;
    }
    execv(argv[1], argv + 1);
    perror("execv");
    return 2;
}
