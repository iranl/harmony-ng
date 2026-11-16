/*
 * test_remote_key_wake.c - the remote's main loop (the real remote_main.c, its main() renamed)
 * on the mock hardware: hard keys arrive on the button pipe, touches on the touch pipe. What the
 * remote does is recorded in order: every write to the radio (the re-sync packet, each frame by
 * type), the unblank of the display and every drawn frame. The unblank and the drawing are slowed
 * down by 40 ms each, as a stand-in for the real display, so the order is beyond doubt.
 *
 * A hard key pressed while the screen is off must reach the Hub first:
 *   radio re-sync, key frame, and only then unblank and first frame.
 * The screen used to be lit first: unblank, two re-syncs, a full frame, then the key frame.
 *
 * Also checked: no second re-sync behind the key frame, a release that is already waiting goes
 * out before the unblank and one that comes during the unblank before the first frame, a touch
 * with the screen off only wakes, and a key with the screen on is sent as before.
 *
 * Run with the mock library and a fresh mock root:
 *   HARMONY_MOCK_DIR=<dir> HARMONY_MOCK_ROLE=remote LD_PRELOAD=libmock_hardware.so ./test_remote_key_wake
 * Linked with -Wl,--wrap=system,--wrap=open,--wrap=settimeofday (the helpers remote_ui starts on
 * the device and a few device nodes must never be touched on a host) and
 * -Wl,--wrap=write,--wrap=ioctl,--wrap=render_screen (the record).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>

#define main remote_ui_main
#include "../payload/source/remote/remote_main.c"
#undef main

#define KEY_VOLUMEUP_CODE 115
#define RF_VOLUMEUP       0x00E9
#define UNBLANK_MS        40
#define RENDER_MS         40

/* ---- the record ---- */
#define MAX_EVENTS 512
static pthread_mutex_t s_ev_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { char what[12]; uint64_t us; } s_ev[MAX_EVENTS];
static int s_ev_count;

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void note(const char *what) {
    pthread_mutex_lock(&s_ev_lock);
    if (s_ev_count < MAX_EVENTS) {
        snprintf(s_ev[s_ev_count].what, sizeof(s_ev[0].what), "%s", what);
        s_ev[s_ev_count].us = now_us();
        s_ev_count++;
    }
    pthread_mutex_unlock(&s_ev_lock);
}

static int mark(void) {
    pthread_mutex_lock(&s_ev_lock);
    int n = s_ev_count;
    pthread_mutex_unlock(&s_ev_lock);
    return n;
}

/* Index of the first `what` at or after `from`, -1 when there is none */
static int first(const char *what, int from) {
    int found = -1;
    pthread_mutex_lock(&s_ev_lock);
    for (int i = from; i < s_ev_count && found < 0; i++) {
        if (strcmp(s_ev[i].what, what) == 0) found = i;
    }
    pthread_mutex_unlock(&s_ev_lock);
    return found;
}

static int count(const char *what, int from) {
    int n = 0;
    pthread_mutex_lock(&s_ev_lock);
    for (int i = from; i < s_ev_count; i++) {
        if (strcmp(s_ev[i].what, what) == 0) n++;
    }
    pthread_mutex_unlock(&s_ev_lock);
    return n;
}

static void order(int from, char *out, size_t len) {
    out[0] = '\0';
    pthread_mutex_lock(&s_ev_lock);
    for (int i = from; i < s_ev_count; i++) {
        size_t used = strlen(out);
        if (used + 16 >= len) break;
        snprintf(out + used, len - used, "%s%s", i > from ? " > " : "", s_ev[i].what);
    }
    pthread_mutex_unlock(&s_ev_lock);
}

/* ---- wrapped calls ---- */
int __wrap_system(const char *cmd) { (void)cmd; return 0; }
int __wrap_settimeofday(const struct timeval *tv, const void *tz) { (void)tv; (void)tz; return 0; }

int __real_open(const char *path, int flags, ...);
int __wrap_open(const char *path, int flags, ...) {
    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list ap;
        va_start(ap, flags);
        mode = (mode_t)va_arg(ap, int);
        va_end(ap);
    }
    /* never the host's own nodes and files */
    if (path && (strcmp(path, "/dev/watchdog") == 0 ||
                 strcmp(path, "/dev/input/event3") == 0 || strcmp(path, "/etc/TZ") == 0 ||
                 strncmp(path, "/var/run/", 9) == 0)) {
        errno = ENOENT;
        return -1;
    }
    return __real_open(path, flags, mode);
}

/* Every write to the radio: the re-sync packet (0x12 ...) or a frame (0x20, mode|slot, 0x12, ...) */
ssize_t __real_write(int fd, const void *buf, size_t n);
ssize_t __wrap_write(int fd, const void *buf, size_t n) {
    if (fd >= 0 && fd == g_rf_fd && buf) {
        const uint8_t *b = (const uint8_t *)buf;
        if (n == 10 && b[0] == 0x12) note("resync");
        else if (n == 32 && b[0] == 0x20 && b[2] == 0x12) {
            if (b[5] == RF_MSG_BUTTON_PRESS) {
                uint16_t key = (uint16_t)((b[7] << 8) | b[8]);
                if (key != RF_VOLUMEUP) note("other-key");
                else note(b[9] == 0x01 ? "press" : (b[9] == 0x02 ? "hold" : "release"));
            } else if (b[5] == RF_MSG_ACTIVITY_REQUEST) note("activity");
            else if (b[5] == RF_MSG_SYNC_REQUEST) note("sync");
            else note("frame");
        }
    }
    return __real_write(fd, buf, n);
}

int __real_ioctl(int fd, unsigned long request, void *arg);
int __wrap_ioctl(int fd, unsigned long request, ...) {
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (request == FBIOBLANK) {
        if ((long)arg == FB_BLANK_UNBLANK) {
            note("unblank");
            usleep(UNBLANK_MS * 1000);
        } else {
            note("blank");
        }
    }
    return __real_ioctl(fd, request, arg);
}

void __real_render_screen(void);
void __wrap_render_screen(void) {
    if (g_fb_mem && g_screen_on) { /* otherwise render_screen() returns without drawing */
        note("render");
        usleep(RENDER_MS * 1000);
    }
    __real_render_screen();
}

/* ---- the test ---- */
static int s_failures = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("  [ok] "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("  [FAIL] "); printf(__VA_ARGS__); printf("\n"); s_failures++; } \
} while (0)

static int s_btn_w = -1;   /* writes to /dev/input/event0 */
static int s_touch_w = -1; /* writes to the touchscreen */

static void *run_remote(void *arg) {
    (void)arg;
    char *argv[] = { "remote_ui", NULL };
    remote_ui_main(1, argv);
    return NULL;
}

static void fill_event(struct input_event *ev, uint16_t type, uint16_t code, int32_t value) {
    memset(ev, 0, sizeof(*ev));
    ev->type = type;
    ev->code = code;
    ev->value = value;
}

/* One key event and its EV_SYN, in one write */
static void key_event(int32_t value) {
    struct input_event ev[2];
    fill_event(&ev[0], EV_KEY, KEY_VOLUMEUP_CODE, value);
    fill_event(&ev[1], EV_SYN, 0, 0);
    if (__real_write(s_btn_w, ev, sizeof(ev)) != (ssize_t)sizeof(ev)) perror("write key event");
}

static void touch_event(uint16_t type, uint16_t code, int32_t value) {
    struct input_event ev;
    fill_event(&ev, type, code, value);
    if (__real_write(s_touch_w, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) perror("write touch event");
}

static bool wait_for(const char *what, int from, int ms) {
    for (int i = 0; i < ms / 2 && first(what, from) < 0; i++) usleep(2000);
    return first(what, from) >= 0;
}

static bool wait_screen(bool on, int ms) {
    for (int i = 0; i < ms / 5 && g_screen_on != on; i++) usleep(5000);
    return g_screen_on == on;
}

/* The screen goes off the way the idle timeout turns it off (the power thread calls this too).
 * The pause lets the main loop finish what it draws and the re-sync hold of the case before end. */
static int screen_off(void) {
    usleep(250000);
    fb_set_blank(true);
    usleep(20000);
    return mark();
}

static bool blank_file_says_on(const char *md) {
    char path[512], buf[8] = "";
    snprintf(path, sizeof(path), "%s/sysfs/fb0_blank", md);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    if (!fgets(buf, sizeof(buf), f)) buf[0] = '\0';
    fclose(f);
    return buf[0] == '0';
}

int main(void) {
    printf("[*] test_remote_key_wake\n");
    const char *md = getenv("HARMONY_MOCK_DIR");
    if (!md || !md[0] || !getenv("LD_PRELOAD")) {
        printf("[-] run with HARMONY_MOCK_DIR=<fresh dir> HARMONY_MOCK_ROLE=remote LD_PRELOAD=libmock_hardware.so\n");
        return 1;
    }
    setenv("HARMONY_MOCK_ROLE", "remote", 1);
    char path[512];
    mkdir(md, 0777);
    snprintf(path, sizeof(path), "%s/sysfs", md);
    mkdir(path, 0777);

    /* the input devices: pipes this test writes events to */
    snprintf(path, sizeof(path), "%s/btn_pipe", md);
    unlink(path);
    if (mkfifo(path, 0666) != 0) { perror("mkfifo btn_pipe"); return 1; }
    s_btn_w = __real_open(path, O_RDWR | O_NONBLOCK);
    snprintf(path, sizeof(path), "%s/touch_pipe", md);
    unlink(path);
    if (mkfifo(path, 0666) != 0) { perror("mkfifo touch_pipe"); return 1; }
    s_touch_w = __real_open(path, O_RDWR | O_NONBLOCK);

    /* the radio: /dev/rfspi is this socket for a process in the remote role */
    struct sockaddr_un sun;
    memset(&sun, 0, sizeof(sun));
    sun.sun_family = AF_UNIX;
    snprintf(sun.sun_path, sizeof(sun.sun_path), "%s/remote_rf.sock", md);
    unlink(sun.sun_path);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0 || bind(srv, (struct sockaddr *)&sun, sizeof(sun)) != 0 || listen(srv, 1) != 0) {
        perror("radio socket");
        return 1;
    }

    pthread_t th;
    pthread_create(&th, NULL, run_remote, NULL);
    int radio = accept(srv, NULL, NULL);
    if (radio < 0) { perror("accept"); return 1; }
    for (int i = 0; i < 600 && g_btn_fd < 0; i++) usleep(10000);
    CHECK(g_btn_fd >= 0 && g_rf_fd >= 0, "remote_ui runs on the mock (buttons and radio open)");
    if (g_btn_fd < 0 || g_rf_fd < 0) return 1;
    g_deep_standby_min = 0;       /* no suspend: this is about the screen */
    g_key_hint_pending = false;   /* the first-use hint takes a key by itself */
    CHECK(wait_screen(true, 2000), "the screen is on after the start");
    usleep(300000);
    g_charging_page_until_ms = 0; /* so does the charging page of a power source seen at the start */

    char seq[512];
    int m, press, unblank, render, resync, release;

    /* 1. A hard key with the screen off: radio, key, then the screen */
    printf("[-] a hard key pressed with the screen off\n");
    m = screen_off();
    CHECK(!g_screen_on, "the screen is off");
    key_event(1);
    wait_for("render", m, 1500);
    usleep(120000);
    order(m, seq, sizeof(seq));
    printf("      order: %s\n", seq);
    press = first("press", m);
    unblank = first("unblank", m);
    render = first("render", m);
    resync = first("resync", m);
    CHECK(count("press", m) == 1, "the press is sent, once (%d press frame(s))", count("press", m));
    CHECK(press >= 0 && unblank >= 0 && press < unblank, "the key frame goes out before the unblank of the display");
    CHECK(press >= 0 && render >= 0 && press < render, "and before the first frame is drawn");
    CHECK(resync >= 0 && press >= 0 && resync < press && s_ev[press].us - s_ev[resync].us >= 4500,
          "the radio is re-synced before the key frame, with the chip's 5 ms between them (%.1f ms)",
          (resync >= 0 && press > resync) ? (s_ev[press].us - s_ev[resync].us) / 1000.0 : -1.0);
    CHECK(press >= 0 && count("resync", press) == 0, "no second re-sync behind the key frame (%d)",
          press >= 0 ? count("resync", press) : -1);
    CHECK(g_screen_on && blank_file_says_on(md) && unblank >= 0 && render > unblank,
          "the screen is on afterwards and a frame was drawn");
    CHECK(g_suspend_hold_until_ms > get_monotonic_ms(), "no suspend can start right behind the key");
    m = mark();
    key_event(0);
    usleep(150000);
    CHECK(count("release", m) == 1 && count("press", m) == 0 && count("hold", press) == 0,
          "the release of the key that woke the screen: one release frame, no hold frame (%d, %d)",
          count("release", m), count("hold", press));

    /* 2. A quick tap, press and release both waiting when the remote reads them */
    printf("[-] a quick tap with the screen off, press and release read together\n");
    m = screen_off();
    {
        struct input_event ev[4];
        fill_event(&ev[0], EV_KEY, KEY_VOLUMEUP_CODE, 1);
        fill_event(&ev[1], EV_SYN, 0, 0);
        fill_event(&ev[2], EV_KEY, KEY_VOLUMEUP_CODE, 0);
        fill_event(&ev[3], EV_SYN, 0, 0);
        if (__real_write(s_btn_w, ev, sizeof(ev)) != (ssize_t)sizeof(ev)) perror("write tap");
    }
    wait_for("render", m, 1500);
    usleep(120000);
    order(m, seq, sizeof(seq));
    printf("      order: %s\n", seq);
    press = first("press", m);
    release = first("release", m);
    unblank = first("unblank", m);
    CHECK(press >= 0 && release > press && unblank > release && count("press", m) == 1 && count("release", m) == 1,
          "press and release both go out before the unblank");
    CHECK(count("hold", m) == 0 && g_screen_on, "no hold frame, and the screen is on");

    /* 3. The release comes while the display is being unblanked */
    printf("[-] the release arrives during the unblank\n");
    m = screen_off();
    key_event(1);
    usleep(15000);
    key_event(0);
    wait_for("render", m, 1500);
    usleep(120000);
    order(m, seq, sizeof(seq));
    printf("      order: %s\n", seq);
    press = first("press", m);
    release = first("release", m);
    unblank = first("unblank", m);
    render = first("render", m);
    CHECK(press >= 0 && unblank > press && release > unblank && render > release,
          "press, unblank, release, and only then the first frame");
    CHECK(count("press", m) == 1 && count("release", m) == 1 && count("hold", m) == 0, "one press, one release, no hold");

    /* 4. A touch with the screen off only wakes */
    printf("[-] a touch with the screen off\n");
    m = screen_off();
    ui_view_t view_before = g_current_view;
    touch_event(EV_ABS, ABS_X, 400);
    touch_event(EV_ABS, ABS_Y, 500);
    touch_event(EV_KEY, 330, 1);
    touch_event(EV_SYN, 0, 0);
    touch_event(EV_KEY, 330, 0);
    touch_event(EV_SYN, 0, 0);
    wait_for("render", m, 1500);
    usleep(120000);
    order(m, seq, sizeof(seq));
    printf("      order: %s\n", seq);
    CHECK(g_screen_on && first("unblank", m) >= 0 && first("render", m) >= 0 && count("resync", m) >= 1,
          "a touch wakes the screen and the radio");
    CHECK(count("press", m) + count("hold", m) + count("release", m) + count("other-key", m) + count("activity", m) == 0 &&
          g_current_view == view_before, "and does nothing else (no key or activity frame, same view)");

    /* 5. A key with the screen on: as before */
    printf("[-] a hard key with the screen on\n");
    usleep(250000);
    m = mark();
    key_event(1);
    usleep(100000);
    key_event(0);
    usleep(150000);
    order(m, seq, sizeof(seq));
    printf("      order: %s\n", seq);
    press = first("press", m);
    release = first("release", m);
    CHECK(count("press", m) == 1 && count("release", m) == 1 && release > press && count("hold", m) == 0,
          "one press frame, then one release frame");
    CHECK(count("unblank", m) == 0 && count("resync", m) == 0, "no wake of the display or the radio");

    g_running = false;
    {
        struct input_event ev;
        fill_event(&ev, EV_SYN, 0, 0); /* ends the main loop's wait */
        if (__real_write(s_btn_w, &ev, sizeof(ev)) != (ssize_t)sizeof(ev)) perror("write");
    }
    pthread_join(th, NULL);
    close(radio);

    if (s_failures) {
        printf("[-] test_remote_key_wake: %d check(s) failed\n", s_failures);
        return 1;
    }
    printf("[+] test_remote_key_wake passed\n");
    return 0;
}
