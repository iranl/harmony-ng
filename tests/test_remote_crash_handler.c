/*
 * test_remote_crash_handler.c - remote_ui's crash handler (the real one in remote_main.c, its
 * main() renamed) may only use async-signal-safe calls: it runs on the thread that faulted, often
 * inside malloc or stdio, and a stdio or localtime call there waits for a lock that thread already
 * holds. The process then hangs instead of exiting and the supervisor never restarts it.
 *
 * fopen() and localtime() are wrapped: called from inside the handler they end the child with
 * exit status 77. The handler must end with 128 + the signal and leave its record in
 * /data/crash.log (on the mock root).
 *
 * Run with the mock library and a fresh mock root:
 *   HARMONY_MOCK_DIR=<dir> LD_PRELOAD=libmock_hardware.so ./test_remote_crash_handler
 * Linked with -Wl,--wrap=fopen,--wrap=localtime,--wrap=system.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <sys/wait.h>

#define main remote_ui_main
#include "../payload/source/remote/remote_main.c"
#undef main

static volatile int s_in_handler;

FILE *__real_fopen(const char *path, const char *mode);
FILE *__wrap_fopen(const char *path, const char *mode) {
    if (s_in_handler) _exit(77);
    return __real_fopen(path, mode);
}

struct tm *__real_localtime(const time_t *t);
struct tm *__wrap_localtime(const time_t *t) {
    if (s_in_handler) _exit(77);
    return __real_localtime(t);
}

int __wrap_system(const char *cmd) { (void)cmd; return 0; }

int main(void) {
    const char *md = getenv("HARMONY_MOCK_DIR");
    char log[400];
    snprintf(log, sizeof(log), "%s/data/crash.log", md && md[0] ? md : "/tmp/harmony_mock");
    unlink(log);
    snprintf(g_active_activity_id, sizeof(g_active_activity_id), "%s", "12");
    g_remote_slot = 1;
    fflush(stdout);

    pid_t pid = fork();
    if (pid == 0) {
        siginfo_t info;
        memset(&info, 0, sizeof(info));
        info.si_code = 1;
        info.si_addr = (void *)0x1234;
        s_in_handler = 1;
        crash_sig_handler(SIGSEGV, &info, NULL);
        _exit(0); /* not reached */
    }
    int status = 0;
    waitpid(pid, &status, 0);
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (code == 77) {
        printf("[-] test_remote_crash_handler: the handler called fopen() or localtime()\n");
        return 1;
    }
    if (code != 128 + SIGSEGV) {
        printf("[-] test_remote_crash_handler: exit status %d, expected %d\n", code, 128 + SIGSEGV);
        return 1;
    }
    char buf[2048] = {0};
    FILE *f = __real_fopen(log, "r");
    size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    if (f) fclose(f);
    buf[n] = '\0';
    if (!strstr(buf, "=== CRASH epoch=") || !strstr(buf, "Signal: 11 (SIGSEGV") || !strstr(buf, "Fault Addr: 0x1234") ||
        !strstr(buf, "act='12'") || !strstr(buf, "VmRSS:")) {
        printf("[-] test_remote_crash_handler: crash record incomplete:\n%s\n", buf);
        return 1;
    }
    printf("[+] test_remote_crash_handler: async-signal-safe record written, exit %d\n", code);
    return 0;
}
