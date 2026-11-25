#define _GNU_SOURCE
#include "plugin_mgr.h"
#include "http_util.h"
#include "config_store.h"
#include "cmd_range.h"
#include "cmd_ir_alt.h"
#include "cJSON.h"
#include "sha256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/statvfs.h>
#include <time.h>
#include <ctype.h>
#include <pthread.h>
#include <sys/wait.h>

extern char **environ;

static uint64_t compute_dir_size_bytes(const char *dir_path) {
    uint64_t total = 0;
    DIR *d = opendir(dir_path);
    if (!d) return 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                total += compute_dir_size_bytes(subpath);
            } else if (S_ISREG(st.st_mode)) {
                total += (uint64_t)st.st_size;
            }
        }
    }
    closedir(d);
    return total;
}

static void format_bytes(uint64_t bytes, char *out, size_t out_sz) {
    if (bytes < 1024) {
        snprintf(out, out_sz, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1024 * 1024) {
        snprintf(out, out_sz, "%.1f KB", (double)bytes / 1024.0);
    } else if (bytes < 1024ULL * 1024 * 1024) {
        snprintf(out, out_sz, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_sz, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    }
}

static int is_safe_plugin_id(const char *id) {
    return plugin_mgr_is_safe_id(id);
}

/* Download limits. /tmp is RAM (30 MB in total on the Hub, a few MB free) and /data is 5 MB of
 * flash shared with the configuration: the largest published plugin archive is 58 KB. */
#define FETCH_ARCHIVE_MAX_BYTES (1L * 1024 * 1024)
#define FETCH_ARCHIVE_MAX_MS    120000
#define FETCH_FILE_MAX_BYTES    (512L * 1024)
#define FETCH_SMALL_MAX_BYTES   (256L * 1024)
#define FETCH_SMALL_MAX_MS      20000

/* A URL handed to wget. URLs come from the network (repositories, manifests) and from API
 * callers: one used to go into a shell command line, where a quote or $( ) ran as root. */
static int is_safe_url(const char *url) {
    if (!url || (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)) return 0;
    if (strlen(url) > 400) return 0;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        if (isalnum(*p) || strchr("-._~:/?#@&+,=%", *p)) continue;
        return 0;
    }
    return 1;
}

/* A file path named in a manifest: relative, inside the plugin directory, plain characters.
 * No part starts with a dot: the copy into place skips dot names, and a "bin/.drv"
 * entrypoint installed as "ok" without its binary. */
static int is_safe_rel_path(const char *f) {
    if (!f || !f[0] || f[0] == '/' || strlen(f) > 200) return 0;
    if (f[0] == '.' || strstr(f, "/.") || strstr(f, "//")) return 0;
    for (const unsigned char *p = (const unsigned char *)f; *p; p++) {
        if (isalnum(*p) || *p == '.' || *p == '_' || *p == '-' || *p == '/') continue;
        return 0;
    }
    return 1;
}

/* hub_fetch (certificate checked), or wget without a shell when the Hub has no hub_fetch,
 * its library or the CA bundle (host tests); the output is removed when the download failed */
static int fetch_url(const char *url, const char *out_path, long max_bytes, unsigned int max_ms) {
    if (!is_safe_url(url)) return -1;
    int checked = fetch_url_checked(url, out_path, max_bytes, max_ms, 10);
    if (checked != RUN_NO_FETCH) return checked;
    /* busybox with its wget applet on the Hub; a plain wget elsewhere (host tests) */
    static const char *const tools[][2] = {
        { "/cache/bin/busybox", "wget" }, { "/data/harmony/bin/busybox", "wget" },
        { "/usr/bin/wget", NULL }, { "/bin/wget", NULL }
    };
    int rc = -1;
    for (size_t i = 0; i < sizeof(tools) / sizeof(tools[0]); i++) {
        if (access(tools[i][0], X_OK) != 0) continue;
        char *argv[11];
        int a = 0;
        argv[a++] = (char *)tools[i][0];
        if (tools[i][1]) argv[a++] = (char *)tools[i][1];
        argv[a++] = "-q";
        argv[a++] = "--no-check-certificate";
        argv[a++] = "-T";
        argv[a++] = "10";
        argv[a++] = "-O";
        argv[a++] = (char *)out_path;
        argv[a++] = (char *)url;
        argv[a] = NULL;
        rc = run_argv_limited(argv, out_path, max_bytes, max_ms);
        if (rc == 0 || rc == RUN_TOO_BIG || rc == RUN_TOO_SLOW) break;
    }
    if (rc != 0) unlink(out_path);
    return rc;
}

static int file_sha256_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&ctx, buf, n);
    int err = ferror(f);
    fclose(f);
    if (err) return -1;
    uint8_t d[32];
    sha256_final(d, &ctx);
    for (int i = 0; i < 32; i++) snprintf(out_hex + 2 * i, 3, "%02x", d[i]);
    return 0;
}

/* Every entry of the archive is a plain file or directory under the unpack directory: a
 * "../" name, an absolute name or a link entry wrote outside it during the unpack */
static int archive_entries_safe(const char *archive_path, const char *list_path, uint64_t *unpacked) {
    *unpacked = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; tar -tvzf \"%s\" > \"%s\" 2>/dev/null",
             archive_path, list_path);
    int ok = (system(cmd) == 0);
    FILE *f = ok ? fopen(list_path, "r") : NULL;
    char line[1024];
    while (ok && f && fgets(line, sizeof(line), f)) {
        if (line[0] != '-' && line[0] != 'd') ok = 0;
        /* BusyBox tar lists a hard link as a plain file ("-rw-r--r-- ... name -> target") */
        if (strstr(line, " -> ")) ok = 0;
        unsigned long long sz = 0;
        if (ok && sscanf(line, "%*s %*s %llu", &sz) == 1) *unpacked += sz;
    }
    if (f) fclose(f);
    if (ok) {
        snprintf(cmd, sizeof(cmd),
                 "export PATH=/cache/bin:/data/harmony/bin:$PATH; tar -tzf \"%s\" > \"%s\" 2>/dev/null",
                 archive_path, list_path);
        ok = (system(cmd) == 0);
        f = ok ? fopen(list_path, "r") : NULL;
        while (ok && f && fgets(line, sizeof(line), f)) {
            line[strcspn(line, "\r\n")] = '\0';
            const char *name = line;
            while (name[0] == '.' && name[1] == '/') name += 2;
            if (name[0] == '/') ok = 0;
            for (const char *c = name; ok && *c; ) {
                size_t len = strcspn(c, "/");
                if (len == 2 && c[0] == '.' && c[1] == '.') ok = 0;
                c += len;
                if (*c == '/') c++;
            }
        }
        if (f) fclose(f);
    }
    unlink(list_path);
    return ok;
}

/* A temp path no other request uses: names from time(NULL) alone collided for two requests in
 * the same second (one removed the other's files) */
static void tmp_name(char *out, size_t out_sz, const char *prefix, const char *suffix) {
    static unsigned int s_seq = 0;
    unsigned int seq = __sync_fetch_and_add(&s_seq, 1);
    snprintf(out, out_sz, "%s_%ld_%d_%u%s", prefix, (long)time(NULL), (int)getpid(), seq, suffix);
}

/* /tmp/plugins/<id>.stopped: stopped by request (stop, disable, uninstall, shutdown). The
 * watchdog leaves such a plugin alone; it used to start it again within 10 s. */
static void stopped_marker_path(const char *plugin_id, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s/%s.stopped", HARMONY_PLUGINS_RUN_DIR, plugin_id);
}

static int is_stopped_by_request(const char *plugin_id) {
    char p[128];
    stopped_marker_path(plugin_id, p, sizeof(p));
    return access(p, F_OK) == 0;
}

/* The plugins hub_daemon started: hub_daemon forks and execs each one itself and keeps its
 * pid. A pid file written by the plugin used to decide what was running and what got killed;
 * a stale one named another process. Only waitpid() on a pid of this table: other threads
 * run system(), and a waitpid(-1) would take their child's status. */
#define MAX_CHILDREN 16
static struct {
    char id[64];
    pid_t pid;
    time_t started;
} s_kids[MAX_CHILDREN];
static pthread_mutex_t s_kids_lock = PTHREAD_MUTEX_INITIALIZER;

/* Plugins with a start or stop under way: a second one for the same id answers busy. Two
 * restarts at once removed the socket the newer instance had just bound. Its own lock:
 * do_start() holds s_kids_lock across fork(). */
static char s_busy[MAX_CHILDREN][64];
static pthread_mutex_t s_busy_lock = PTHREAD_MUTEX_INITIALIZER;

static int busy_take(const char *id) {
    int slot = -1;
    pthread_mutex_lock(&s_busy_lock);
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (strcmp(s_busy[i], id) == 0) {
            slot = -2;
            break;
        }
        if (!s_busy[i][0] && slot == -1) slot = i;
    }
    if (slot >= 0) snprintf(s_busy[slot], sizeof(s_busy[slot]), "%s", id);
    pthread_mutex_unlock(&s_busy_lock);
    return slot >= 0;
}

static void busy_drop(const char *id) {
    pthread_mutex_lock(&s_busy_lock);
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (strcmp(s_busy[i], id) == 0) s_busy[i][0] = '\0';
    }
    pthread_mutex_unlock(&s_busy_lock);
}

/* The environment handed to plugins, copied once at init: walking environ (or getenv) at
 * each start raced with setenv("TZ") of a settings save on another thread. TZ is left out:
 * uClibc reads /etc/TZ, which the settings save keeps current. */
static char **s_base_env;
static char *s_env_path; /* "PATH=/cache/bin:/data/harmony/bin:<inherited PATH>" */
/* "LD_LIBRARY_PATH=<HARMONY_PLUGIN_LIB_DIR>[:<inherited>]": the shared libraries for plugins
 * (libbearssl.so.0). The stock ld-uClibc 0.9.30 ignores DT_RPATH, so -Wl,-rpath alone does
 * not find them (checked on a Hub: "can't load library"). */
static char *s_env_ldpath;

static void snapshot_env(void) {
    if (s_base_env) return;
    size_t n = 0;
    while (environ && environ[n]) n++;
    char **e = calloc(n + 1, sizeof(char *));
    if (!e) return;
    size_t k = 0;
    const char *old_path = NULL, *old_ld = NULL;
    for (size_t i = 0; i < n; i++) {
        const char *v = environ[i];
        if (strncmp(v, "PATH=", 5) == 0) old_path = v + 5;
        if (strncmp(v, "LD_LIBRARY_PATH=", 16) == 0) old_ld = v + 16;
        if (strncmp(v, "PATH=", 5) == 0 || strncmp(v, "HARMONY_", 8) == 0 || strncmp(v, "TZ=", 3) == 0 ||
            strncmp(v, "LD_LIBRARY_PATH=", 16) == 0) continue;
        if ((e[k] = strdup(v)) != NULL) k++;
    }
    /* sized to the inherited PATH: a fixed buffer cut a long one before /usr/bin */
    if (!old_path || !old_path[0]) old_path = "/bin:/sbin:/usr/bin:/usr/sbin";
    size_t path_len = strlen(old_path) + sizeof("PATH=/cache/bin:/data/harmony/bin:");
    if ((s_env_path = malloc(path_len)) != NULL) snprintf(s_env_path, path_len, "PATH=/cache/bin:/data/harmony/bin:%s", old_path);
    size_t ld_len = (old_ld ? strlen(old_ld) : 0) + sizeof("LD_LIBRARY_PATH=" HARMONY_PLUGIN_LIB_DIR ":");
    if ((s_env_ldpath = malloc(ld_len)) != NULL)
        snprintf(s_env_ldpath, ld_len, "LD_LIBRARY_PATH=%s%s%s", HARMONY_PLUGIN_LIB_DIR,
                 old_ld && old_ld[0] ? ":" : "", old_ld && old_ld[0] ? old_ld : "");
    s_base_env = e;
}

/* The id of a plugin this hub_daemon started, from the process's environment: every plugin
 * it starts has HARMONY_PLUGIN_ID=<id> and HARMONY_SOCKET=<run folder>/<id>.sock, and its
 * helpers inherit both. A program started by hand in a plugin folder has neither (the old
 * sweep by working directory killed those, and the shell that ran init.sh from there); a
 * shell that only exported HARMONY_PLUGIN_ID, or the plugins of another run folder (a second
 * test daemon), do not have this socket. */
static int proc_plugin_id(pid_t pid, char *id_out, size_t id_sz) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/environ", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    const size_t cap = 32768;
    char *buf = malloc(cap);
    size_t len = 0;
    ssize_t r;
    while (buf && len < cap - 1 && (r = read(fd, buf + len, cap - 1 - len)) > 0) len += (size_t)r;
    close(fd);
    if (!buf) return 0;
    buf[len] = '\0';
    const char *idv = NULL, *sockv = NULL;
    for (size_t off = 0; off < len; off += strlen(buf + off) + 1) {
        const char *v = buf + off;
        if (strncmp(v, "HARMONY_PLUGIN_ID=", 18) == 0) idv = v + 18;
        else if (strncmp(v, "HARMONY_SOCKET=", 15) == 0) sockv = v + 15;
    }
    int hit = 0;
    if (idv && sockv && is_safe_plugin_id(idv)) {
        char want[200];
        snprintf(want, sizeof(want), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, idv);
        hit = strcmp(sockv, want) == 0;
        if (hit) snprintf(id_out, id_sz, "%s", idv);
    }
    free(buf);
    return hit;
}

static pid_t read_ppid(pid_t pid) {
    char path[64], st[256];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    int ok = fgets(st, sizeof(st), f) != NULL;
    fclose(f);
    const char *p = ok ? strrchr(st, ')') : NULL;
    int ppid = 0;
    if (!p || sscanf(p + 1, " %*c %d", &ppid) != 1) return 0;
    return (pid_t)ppid;
}

/* 1 when pgid is the process group of a plugin a slot tracks (its leader's pid) */
static int tracked_group(pid_t pgid) {
    int hit = 0;
    pthread_mutex_lock(&s_kids_lock);
    for (int i = 0; i < MAX_CHILDREN && pgid > 0; i++) {
        if (s_kids[i].pid == pgid) hit = 1;
    }
    pthread_mutex_unlock(&s_kids_lock);
    return hit;
}

/* Time a plugin process has after its SIGTERM before the SIGKILL: the EXIT trap of the
 * ps5-wake helper restores the Hub's Bluetooth address and removes its lock, about 5 s */
#ifndef PLUGIN_TERM_GRACE_S
#define PLUGIN_TERM_GRACE_S 8
#endif

/* Plugin processes no slot tracks that were sent SIGTERM, and when the SIGKILL is due */
#define MAX_LEFTOVERS 32
static struct {
    pid_t pid;
    time_t kill_at;
} s_left[MAX_LEFTOVERS];
static pthread_mutex_t s_left_lock = PTHREAD_MUTEX_INITIALIZER;

/* Seconds on the monotonic clock: the SNTP step from 1970 does not cut or stretch the grace */
static time_t mono_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* Plugin processes no slot tracks: helpers left when a plugin's leader died, a copy that
 * daemonized, the plugins of a hub_daemon that was killed. Each gets one SIGTERM, and the
 * SIGKILL PLUGIN_TERM_GRACE_S later from plugin_mgr_tick(): nothing waits here, and this
 * runs on the main loop (a watchdog restart). A process of the group `signalled` (a stop just
 * sent it SIGTERM) gets no second one: that cut its EXIT trap short. hub_daemon and its
 * ancestors are never taken (a shell with the plugin environment that started it), nor a
 * running instance a slot tracks. id NULL: every plugin's. */
static void sweep_leftovers(const char *id, pid_t signalled) {
    pid_t mine[16];
    int n_mine = 0;
    for (pid_t p = getpid(); p > 1 && n_mine < 16; p = read_ppid(p)) mine[n_mine++] = p;
    pid_t found[64], group[64];
    int n = 0;
    DIR *d = opendir("/proc");
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < 64) {
        if (de->d_name[0] < '1' || de->d_name[0] > '9') continue;
        pid_t pid = (pid_t)atoi(de->d_name);
        int skip = pid <= 1;
        for (int i = 0; i < n_mine && !skip; i++) skip = pid == mine[i];
        char pid_id[64];
        if (skip || !proc_plugin_id(pid, pid_id, sizeof(pid_id)) || (id && strcmp(pid_id, id) != 0)) continue;
        pid_t g = getpgid(pid);
        if (tracked_group(g)) continue;
        group[n] = g;
        found[n++] = pid;
    }
    closedir(d);
    time_t now = mono_s();
    pthread_mutex_lock(&s_left_lock);
    for (int i = 0; i < n; i++) {
        int j, free_j = -1;
        for (j = 0; j < MAX_LEFTOVERS && s_left[j].pid != found[i]; j++) {
            if (s_left[j].pid <= 0 && free_j < 0) free_j = j;
        }
        if (j < MAX_LEFTOVERS) continue; /* signalled already, SIGKILL when due */
        if (signalled <= 0 || group[i] != signalled) {
            printf("[*] Stopping plugin process %d that hub_daemon does not track (%s)\n", (int)found[i], id ? id : "any plugin");
            kill(found[i], SIGTERM);
        }
        if (free_j >= 0) {
            s_left[free_j].pid = found[i];
            s_left[free_j].kill_at = now + PLUGIN_TERM_GRACE_S;
        } else {
            kill(found[i], SIGKILL); /* table full */
        }
    }
    pthread_mutex_unlock(&s_left_lock);
}

/* SIGKILL to the leftovers whose grace has passed (all of them with force), when they still
 * have the plugin environment: a pid that ended and was reused is not taken. Returns how many
 * are still waiting. */
static int reap_leftovers(int force) {
    int waiting = 0;
    time_t now = mono_s();
    pthread_mutex_lock(&s_left_lock);
    for (int j = 0; j < MAX_LEFTOVERS; j++) {
        pid_t pid = s_left[j].pid;
        if (pid <= 0) continue;
        char pid_id[64];
        if (kill(pid, 0) != 0 || !proc_plugin_id(pid, pid_id, sizeof(pid_id))) {
            s_left[j].pid = 0;
        } else if (force || now >= s_left[j].kill_at) {
            printf("[*] Plugin process %d (%s) still there after SIGTERM: SIGKILL\n", (int)pid, pid_id);
            kill(pid, SIGKILL);
            s_left[j].pid = 0;
        } else {
            waiting++;
        }
    }
    pthread_mutex_unlock(&s_left_lock);
    return waiting;
}

static int kid_find_locked(const char *id) {
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (s_kids[i].pid > 0 && strcmp(s_kids[i].id, id) == 0) return i;
    }
    return -1;
}

/* 1 while the child of slot i runs; reaps it and frees the slot once it has exited */
static int kid_alive_locked(int i) {
    int status = 0;
    pid_t r = waitpid(s_kids[i].pid, &status, WNOHANG);
    if (r == 0) return 1;
    /* not reapable here (a thread library that gives each thread its own pid): trust the
     * signal check rather than start a second copy */
    if (r < 0 && errno == ECHILD && kill(s_kids[i].pid, 0) == 0) return 1;
    if (r == s_kids[i].pid) {
        if (WIFEXITED(status)) {
            printf("[!] Plugin '%s' (pid %d) exited with status %d\n", s_kids[i].id, (int)r, WEXITSTATUS(status));
        } else if (WIFSIGNALED(status)) {
            printf("[!] Plugin '%s' (pid %d) killed by signal %d\n", s_kids[i].id, (int)r, WTERMSIG(status));
        }
    }
    s_kids[i].pid = 0;
    s_kids[i].id[0] = '\0';
    return 0;
}

static pid_t running_pid(const char *plugin_id) {
    pid_t pid = 0;
    pthread_mutex_lock(&s_kids_lock);
    int i = kid_find_locked(plugin_id);
    if (i >= 0 && kid_alive_locked(i)) pid = s_kids[i].pid;
    pthread_mutex_unlock(&s_kids_lock);
    return pid;
}

int plugin_mgr_is_running(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return 0;
    return running_pid(plugin_id) > 0;
}

static int is_plugin_enabled(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return 0;
    char dis_path[256];
    snprintf(dis_path, sizeof(dis_path), "%s/%s/disabled", HARMONY_PLUGINS_DIR, plugin_id);
    return (access(dis_path, F_OK) != 0);
}

static int do_start(const char *plugin_id);
static int do_stop(const char *plugin_id);

int plugin_mgr_set_enabled(const char *plugin_id, int enabled) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    char dis_path[256];
    snprintf(dis_path, sizeof(dis_path), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    if (access(dis_path, F_OK) != 0) return PLUGIN_ERR_NOT_FOUND;
    snprintf(dis_path, sizeof(dis_path), "%s/%s/disabled", HARMONY_PLUGINS_DIR, plugin_id);
    /* busy first: a disable during a restart wrote the marker, answered busy and left the
     * plugin running, then not started after the next boot */
    if (!busy_take(plugin_id)) return PLUGIN_ERR_BUSY;
    int rc;
    if (enabled) {
        unlink(dis_path);
        rc = do_start(plugin_id);
    } else {
        int fd = open(dis_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
        rc = do_stop(plugin_id);
    }
    busy_drop(plugin_id);
    return rc;
}

/* The manifest's entrypoint, checked: a relative path inside the plugin directory */
static int read_entrypoint(const char *plug_dir, char *out, size_t out_sz) {
    char mpath[300];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", plug_dir);
    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    if (!mtxt) return -1;
    cJSON *root = cJSON_Parse(mtxt);
    free(mtxt);
    if (!root) return -1;
    const cJSON *j_ep = cJSON_GetObjectItemCaseSensitive(root, "entrypoint");
    int ok = cJSON_IsString(j_ep) && is_safe_rel_path(j_ep->valuestring);
    if (ok) snprintf(out, out_sz, "%s", j_ep->valuestring);
    cJSON_Delete(root);
    return ok ? 0 : -1;
}

/* Start and stop with the plugin's busy flag held by the caller */
static int do_start(const char *plugin_id) {
    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    if (access(plug_dir, F_OK) != 0) return PLUGIN_ERR_NOT_FOUND;
    if (running_pid(plugin_id) > 0) return 0;
    /* what an earlier instance left: else each restart of a plugin that forks added a copy */
    sweep_leftovers(plugin_id, 0);

    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
    char marker[128];
    stopped_marker_path(plugin_id, marker, sizeof(marker));
    unlink(marker);

    /* State that survives an update: the plugin directory is replaced on every install */
    char data_dir[256];
    snprintf(data_dir, sizeof(data_dir), "%s/%s", HARMONY_PLUGINS_DATA_DIR, plugin_id);
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGINS_DATA_DIR, 0755);
    mkdir(data_dir, 0700);

    char entry[208];
    if (read_entrypoint(plug_dir, entry, sizeof(entry)) != 0) return -1;

    /* Everything the child needs is built here: between fork() and exec() of a threaded
     * process only async-signal-safe calls are allowed (no malloc, no printf, no setenv) */
    char exe[512], log_path[160], sock_path[128];
    snprintf(exe, sizeof(exe), "%s/%s", plug_dir, entry);
    snprintf(log_path, sizeof(log_path), "%s/%s.log", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    if (access(exe, X_OK) != 0) return -1;

    char e_id[96], e_sock[160], e_data[288];
    snprintf(e_id, sizeof(e_id), "HARMONY_PLUGIN_ID=%s", plugin_id);
    snprintf(e_sock, sizeof(e_sock), "HARMONY_SOCKET=%s", sock_path);
    snprintf(e_data, sizeof(e_data), "HARMONY_PLUGIN_DATA=%s", data_dir);
    snapshot_env();
    if (!s_env_path || !s_env_ldpath) return -1;
    size_t n_env = 0;
    while (s_base_env && s_base_env[n_env]) n_env++;
    char **envp = calloc(n_env + 6, sizeof(char *));
    if (!envp) return -1;
    size_t k = 0;
    for (size_t i = 0; i < n_env; i++) envp[k++] = s_base_env[i];
    envp[k++] = s_env_path;
    envp[k++] = s_env_ldpath;
    envp[k++] = e_id;
    envp[k++] = e_sock;
    envp[k++] = e_data;
    envp[k] = NULL;
    char *argv[] = { exe, sock_path, NULL };
    /* hub_daemon ignores SIGPIPE: a plugin inherited that, and a shell plugin whose pipe
     * reader had exited looped on EPIPE */
    struct sigaction sa_dfl;
    memset(&sa_dfl, 0, sizeof(sa_dfl));
    sa_dfl.sa_handler = SIG_DFL;
    sigset_t no_mask;
    sigemptyset(&no_mask);

    int rc = 0;
    pthread_mutex_lock(&s_kids_lock);
    int slot = kid_find_locked(plugin_id);
    if (slot >= 0 && kid_alive_locked(slot)) {
        pthread_mutex_unlock(&s_kids_lock);
        free(envp);
        return 0; /* already running */
    }
    for (slot = 0; slot < MAX_CHILDREN && s_kids[slot].pid > 0; slot++) {}
    if (slot >= MAX_CHILDREN) {
        rc = -1;
    } else {
        unlink(sock_path);
        pid_t pid = fork();
        if (pid == 0) {
            /* own session and process group: a stop signals the plugin and its helpers */
            setsid();
            for (int s = 1; s < 32; s++) sigaction(s, &sa_dfl, NULL);
            sigprocmask(SIG_SETMASK, &no_mask, NULL);
            if (chdir(plug_dir) != 0) _exit(126);
            int nul = open("/dev/null", O_RDONLY);
            /* O_APPEND: the watchdog empties a log that grows too large (/tmp is RAM) */
            int lg = open(log_path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
            if (nul >= 0) dup2(nul, 0);
            if (lg >= 0) { dup2(lg, 1); dup2(lg, 2); }
            /* hub_daemon's sockets and files stay with hub_daemon */
            for (int fd = 3; fd < 1024; fd++) close(fd);
            execve(exe, argv, envp);
            _exit(127);
        }
        if (pid < 0) {
            rc = -1;
        } else {
            snprintf(s_kids[slot].id, sizeof(s_kids[slot].id), "%s", plugin_id);
            s_kids[slot].pid = pid;
            s_kids[slot].started = time(NULL);
        }
    }
    pthread_mutex_unlock(&s_kids_lock);
    free(envp);
    return rc;
}

/* SIGTERM to each plugin's process group, up to 1 s for the leaders to exit, then SIGKILL to
 * the leaders that did not. Only the leaders: a helper of the group keeps its SIGTERM and
 * finishes its EXIT trap (ps5-wake restores the Bluetooth address, about 5 s); the caller
 * hands what is left of the group to the leftover table (SIGKILL after the grace). All at
 * once: one by one, each plugin that ignores SIGTERM cost 1 s of init.sh's 2 s. */
static void stop_pids(pid_t *pids, int n) {
    int status;
    for (int i = 0; i < n; i++) {
        kill(-pids[i], SIGTERM);
        kill(pids[i], SIGTERM);
    }
    for (int round = 0; round < 2; round++) {
        for (int t = 0; t < 20; t++) {
            int left = 0;
            for (int i = 0; i < n; i++) {
                if (pids[i] > 0 && waitpid(pids[i], &status, WNOHANG) != 0) pids[i] = 0;
                if (pids[i] > 0) left++;
            }
            if (!left) return;
            usleep(50000);
        }
        for (int i = 0; round == 0 && i < n; i++) {
            if (pids[i] > 0) kill(pids[i], SIGKILL);
        }
    }
}

static void write_stopped_marker(const char *plugin_id) {
    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
    char marker[128];
    stopped_marker_path(plugin_id, marker, sizeof(marker));
    int mfd = open(marker, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (mfd >= 0) close(mfd);
}

/* Frees the plugin's slot and returns its pid (0 when none) */
static pid_t take_kid(const char *plugin_id) {
    pid_t pid = 0;
    pthread_mutex_lock(&s_kids_lock);
    int i = kid_find_locked(plugin_id);
    if (i >= 0) {
        pid = s_kids[i].pid;
        s_kids[i].pid = 0;
        s_kids[i].id[0] = '\0';
    }
    pthread_mutex_unlock(&s_kids_lock);
    return pid;
}

static int do_stop(const char *plugin_id) {
    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    pid_t pid = take_kid(plugin_id);
    /* not installed: nothing to stop, and no marker left in /tmp for any id a client names */
    if (pid <= 0 && access(plug_dir, F_OK) != 0) return PLUGIN_ERR_NOT_FOUND;
    write_stopped_marker(plugin_id);

    char sock_path[128];
    snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    unlink(sock_path);
    pid_t group = pid; /* the plugin's process group (setsid) */
    if (pid > 1) stop_pids(&pid, 1);
    /* with no slot too: helpers of a crashed leader, a copy that daemonized. The helpers of
     * this group got their SIGTERM above and finish their EXIT trap. */
    sweep_leftovers(plugin_id, group);
    unlink(sock_path);
    return 0;
}

int plugin_mgr_start(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    if (!busy_take(plugin_id)) return PLUGIN_ERR_BUSY;
    int rc = do_start(plugin_id);
    busy_drop(plugin_id);
    return rc;
}

int plugin_mgr_stop(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    if (!busy_take(plugin_id)) return PLUGIN_ERR_BUSY;
    int rc = do_stop(plugin_id);
    busy_drop(plugin_id);
    return rc;
}

int plugin_mgr_restart(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    if (!busy_take(plugin_id)) return PLUGIN_ERR_BUSY;
    int rc = do_stop(plugin_id);
    if (rc == 0) rc = do_start(plugin_id);
    busy_drop(plugin_id);
    return rc;
}

static void delete_dir_recursive(const char *dir_path) {
    if (!dir_path || !dir_path[0]) return;
    DIR *d = opendir(dir_path);
    if (!d) {
        unlink(dir_path);
        rmdir(dir_path);
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char subpath[512];
        snprintf(subpath, sizeof(subpath), "%s/%s", dir_path, de->d_name);
        struct stat st;
        if (lstat(subpath, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                delete_dir_recursive(subpath);
            } else {
                unlink(subpath);
            }
        }
    }
    closedir(d);
    rmdir(dir_path);
}

/* Copies a plugin's files; -1 at the first file that cannot be written in full (flash full).
 * Every file is made executable, as the old "chmod -R 755" did: the entrypoint must be. */
static int copy_dir_recursive(const char *src_dir, const char *dst_dir) {
    if (!src_dir || !dst_dir) return -1;
    if (mkdir(dst_dir, 0755) != 0 && errno != EEXIST) return -1;
    DIR *d = opendir(src_dir);
    if (!d) return -1;
    int rc = 0;
    struct dirent *de;
    while (rc == 0 && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        if (de->d_name[0] == '.') {
            printf("[!] Plugin file %s/%s not installed: names starting with a dot are skipped\n", src_dir, de->d_name);
            continue;
        }
        char s_path[512], d_path[512];
        snprintf(s_path, sizeof(s_path), "%s/%s", src_dir, de->d_name);
        snprintf(d_path, sizeof(d_path), "%s/%s", dst_dir, de->d_name);
        struct stat st;
        if (lstat(s_path, &st) != 0) { rc = -1; break; }
        if (S_ISDIR(st.st_mode)) {
            rc = copy_dir_recursive(s_path, d_path);
        } else if (S_ISREG(st.st_mode)) {
            FILE *sf = fopen(s_path, "rb");
            FILE *df = sf ? fopen(d_path, "wb") : NULL;
            if (!sf || !df) rc = -1;
            char buf[4096];
            size_t n;
            while (rc == 0 && (n = fread(buf, 1, sizeof(buf), sf)) > 0) {
                if (fwrite(buf, 1, n, df) != n) rc = -1;
            }
            if (rc == 0 && ferror(sf)) rc = -1;
            if (df && fclose(df) != 0) rc = -1;
            if (sf) fclose(sf);
            if (rc == 0 && chmod(d_path, 0755) != 0) rc = -1;
        }
    }
    closedir(d);
    return rc;
}

/* Space kept free on /data for the configuration: an install that leaves less is refused */
#ifndef PLUGIN_FLASH_RESERVE
#define PLUGIN_FLASH_RESERVE (256ULL * 1024)
#endif

static uint64_t free_bytes_at(const char *path) {
    struct statvfs vfs;
    if (statvfs(path, &vfs) != 0) return 0;
    return (uint64_t)vfs.f_bavail * vfs.f_frsize;
}

/* a host test points these at /proc (0 blocks, like ramfs) and a meminfo file of its own */
#ifndef PLUGIN_TMP_STATFS_PATH
#define PLUGIN_TMP_STATFS_PATH "/tmp"
#endif
#ifndef PLUGIN_MEMINFO_PATH
#define PLUGIN_MEMINFO_PATH "/proc/meminfo"
#endif

/* Free RAM for an unpack in /tmp. The Hub's /tmp is ramfs, which reports 0 blocks in total
 * (and 0 free): statvfs refused every install there. Without a size, MemFree plus half of
 * Cached from /proc/meminfo. */
static uint64_t tmp_free_bytes(void) {
    struct statvfs vfs;
    if (statvfs(PLUGIN_TMP_STATFS_PATH, &vfs) == 0 && vfs.f_blocks > 0) return (uint64_t)vfs.f_bavail * vfs.f_frsize;
    FILE *f = fopen(PLUGIN_MEMINFO_PATH, "r");
    if (!f) return 0;
    unsigned long long mem_free = 0, cached = 0, v;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemFree: %llu", &v) == 1) mem_free = v;
        else if (sscanf(line, "Cached: %llu", &v) == 1) cached = v;
    }
    fclose(f);
    return (mem_free + cached / 2) * 1024ULL;
}

/* One install, pull or uninstall at a time: two at once ran the copy and the removal on the
 * same directory */
static pthread_mutex_t s_install_lock = PTHREAD_MUTEX_INITIALIZER;

int plugin_mgr_uninstall(const char *plugin_id) {
    if (!is_safe_plugin_id(plugin_id)) return -1;
    char plug_dir[256];
    snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, plugin_id);
    struct stat st;
    if (stat(plug_dir, &st) != 0 || !S_ISDIR(st.st_mode)) return PLUGIN_ERR_NOT_FOUND;
    if (pthread_mutex_trylock(&s_install_lock) != 0) return PLUGIN_ERR_BUSY;
    /* no start of this plugin (watchdog, restart) while its folder goes */
    if (!busy_take(plugin_id)) {
        pthread_mutex_unlock(&s_install_lock);
        return PLUGIN_ERR_BUSY;
    }

    do_stop(plugin_id);
    delete_dir_recursive(plug_dir);
    busy_drop(plugin_id);

    /* Its state (pairing keys) and its log go too */
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", HARMONY_PLUGINS_DATA_DIR, plugin_id);
    delete_dir_recursive(path);
    snprintf(path, sizeof(path), "%s/%s.log", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    unlink(path);
    snprintf(path, sizeof(path), "%s/%s.status", HARMONY_PLUGINS_RUN_DIR, plugin_id);
    unlink(path);
    stopped_marker_path(plugin_id, path, sizeof(path));
    unlink(path);
    pthread_mutex_unlock(&s_install_lock);
    return 0;
}

/* Writes where a plugin came from, for a later pull:
 * {"url": ..., "dev": true|false, "repository": <repository.json URL of its catalog entry>} */
static void write_source(const char *dir, const char *url, const char *repo, int dev) {
    if (!url || !url[0]) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "url", url);
    cJSON_AddBoolToObject(o, "dev", dev);
    if (repo && repo[0]) cJSON_AddStringToObject(o, "repository", repo);
    char *txt = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (!txt) return;
    char path[300];
    snprintf(path, sizeof(path), "%s/.source.json", dir);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(txt, f);
        fclose(f);
    }
    free(txt);
}

/* Puts the files of src_dir in place of the installed plugin. The new version is copied
 * next to the old one first: a copy that fails (flash full) leaves the old version as it was.
 * A plugin that was disabled stays disabled: the marker was inside the replaced directory. */
static int replace_plugin_dir(const char *plug_id, const char *src_dir, const char *source_url,
                              const char *source_repo, int dev, char *err_msg, size_t err_msg_sz) {
    char dest_dir[256], new_dir[256], old_dir[256];
    snprintf(dest_dir, sizeof(dest_dir), "%s/%s", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(new_dir, sizeof(new_dir), "%s/.%s.new", HARMONY_PLUGINS_DIR, plug_id);
    snprintf(old_dir, sizeof(old_dir), "%s/.%s.old", HARMONY_PLUGINS_DIR, plug_id);
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGINS_DIR, 0755);
    delete_dir_recursive(new_dir);
    delete_dir_recursive(old_dir);

    uint64_t need = compute_dir_size_bytes(src_dir);
    uint64_t avail = free_bytes_at(HARMONY_PLUGINS_DIR);
    if (need + PLUGIN_FLASH_RESERVE > avail) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Not enough flash space: the plugin needs %llu KB, %llu KB free (%llu KB kept for the configuration)",
                              (unsigned long long)(need / 1024), (unsigned long long)(avail / 1024),
                              (unsigned long long)(PLUGIN_FLASH_RESERVE / 1024));
        return -1;
    }
    if (copy_dir_recursive(src_dir, new_dir) != 0) {
        delete_dir_recursive(new_dir);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to write the plugin files (flash full?); the installed version is unchanged");
        return -1;
    }
    write_source(new_dir, source_url, source_repo, dev);

    struct stat st;
    int had_old = (stat(dest_dir, &st) == 0);
    int was_disabled = had_old && !is_plugin_enabled(plug_id);
    if (was_disabled) {
        char dis_path[300];
        snprintf(dis_path, sizeof(dis_path), "%s/disabled", new_dir);
        int fd = open(dis_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) close(fd);
    }

    /* no start of this plugin (watchdog, restart) between the two renames */
    if (!busy_take(plug_id)) {
        delete_dir_recursive(new_dir);
        if (err_msg) snprintf(err_msg, err_msg_sz, "The plugin is being started or stopped; try again");
        return PLUGIN_ERR_BUSY;
    }
    do_stop(plug_id);
    int rc = 0;
    if (had_old && rename(dest_dir, old_dir) != 0) {
        delete_dir_recursive(new_dir);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to replace the installed version");
        rc = -1;
    } else if (rename(new_dir, dest_dir) != 0) {
        if (had_old) rename(old_dir, dest_dir);
        delete_dir_recursive(new_dir);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to put the new version in place");
        rc = -1;
    } else {
        delete_dir_recursive(old_dir);
    }
    if (!was_disabled) do_start(plug_id);
    busy_drop(plug_id);
    return rc;
}

/* A power cut inside replace_plugin_dir() leaves .<id>.new and .<id>.old, and between the two
 * renames no <id>. .<id>.new is complete only then (.<id>.old exists): it is written in full
 * before the first rename. Without .<id>.old it is a first install cut during its copy
 * (half the files, maybe a cut script) and goes. Puts a version back in place, then removes
 * what is left. */
static void recover_replaced_dirs(void) {
    char names[32][80];
    int n = 0;
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL && n < 32) {
        size_t l = strlen(de->d_name);
        if (de->d_name[0] != '.' || l < 6 || l >= sizeof(names[0])) continue;
        if (strcmp(de->d_name + l - 4, ".new") != 0 && strcmp(de->d_name + l - 4, ".old") != 0) continue;
        snprintf(names[n++], sizeof(names[0]), "%s", de->d_name);
    }
    closedir(d);
    for (int i = 0; i < n; i++) {
        char id[80];
        snprintf(id, sizeof(id), "%.*s", (int)strlen(names[i]) - 5, names[i] + 1);
        if (!is_safe_plugin_id(id)) continue;
        char dest[256], nw[256], old[256];
        snprintf(dest, sizeof(dest), "%s/%s", HARMONY_PLUGINS_DIR, id);
        snprintf(nw, sizeof(nw), "%s/.%s.new", HARMONY_PLUGINS_DIR, id);
        snprintf(old, sizeof(old), "%s/.%s.old", HARMONY_PLUGINS_DIR, id);
        struct stat st;
        if (stat(dest, &st) != 0 && stat(old, &st) == 0) {
            if ((stat(nw, &st) == 0 && rename(nw, dest) == 0) || rename(old, dest) == 0) {
                printf("[*] Plugin '%s' put back in place after an interrupted install\n", id);
            }
        }
        snprintf(dest, sizeof(dest), "%s/%s", HARMONY_PLUGINS_DIR, names[i]);
        delete_dir_recursive(dest);
    }
}

int plugin_mgr_init(void) {
    mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGINS_DIR, 0755);
    snapshot_env();
    /* the plugins of a hub_daemon that was killed (kill -9, crash) are no child of this one:
     * SIGTERM now, SIGKILL from plugin_mgr_tick() after the grace. No wait: this runs after
     * the sockets listen, and a helper's EXIT trap held the first answer up to 8 s. The new
     * copies start meanwhile, as at a watchdog restart. */
    sweep_leftovers(NULL, 0);
    recover_replaced_dirs();

    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return 0;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        if (is_plugin_enabled(de->d_name)) {
            plugin_mgr_start(de->d_name);
        }
    }
    closedir(d);
    return 0;
}

void plugin_mgr_shutdown(void) {
    char ids[MAX_CHILDREN][64];
    pid_t pids[MAX_CHILDREN];
    int n = 0;
    pthread_mutex_lock(&s_kids_lock);
    for (int i = 0; i < MAX_CHILDREN; i++) {
        if (s_kids[i].pid <= 0) continue;
        memcpy(ids[n], s_kids[i].id, sizeof(ids[0]));
        pids[n++] = s_kids[i].pid;
        s_kids[i].pid = 0;
        s_kids[i].id[0] = '\0';
    }
    pthread_mutex_unlock(&s_kids_lock);
    for (int i = 0; i < n; i++) write_stopped_marker(ids[i]);
    stop_pids(pids, n);
    for (int i = 0; i < n; i++) {
        char sock_path[128];
        snprintf(sock_path, sizeof(sock_path), "%s/%s.sock", HARMONY_PLUGINS_RUN_DIR, ids[i]);
        unlink(sock_path);
    }
}

/* Restarts of a plugin that keeps dying: 20 s, 40 s, ... up to 5 min apart */
#define WD_SLOTS 16
#define WD_MAX_DELAY_S 300
#define PLUGIN_LOG_MAX_BYTES (256 * 1024)
static struct {
    char id[64];
    int restarts;
    time_t next_try;
} s_wd[WD_SLOTS];

static int wd_slot(const char *id) {
    int free_slot = -1;
    for (int i = 0; i < WD_SLOTS; i++) {
        if (strcmp(s_wd[i].id, id) == 0) return i;
        if (!s_wd[i].id[0] && free_slot < 0) free_slot = i;
    }
    /* full: the slot of a plugin that is no longer installed (slots were never freed, and
     * after 16 ids a crashing plugin restarted every 10 s without backoff) */
    for (int i = 0; free_slot < 0 && i < WD_SLOTS; i++) {
        char dir[256];
        snprintf(dir, sizeof(dir), "%s/%s", HARMONY_PLUGINS_DIR, s_wd[i].id);
        if (access(dir, F_OK) != 0) free_slot = i;
    }
    if (free_slot >= 0) {
        snprintf(s_wd[free_slot].id, sizeof(s_wd[free_slot].id), "%s", id);
        s_wd[free_slot].restarts = 0;
        s_wd[free_slot].next_try = 0;
    }
    return free_slot;
}

static int wd_restarts(const char *id) {
    for (int i = 0; i < WD_SLOTS; i++) {
        if (strcmp(s_wd[i].id, id) == 0) return s_wd[i].restarts;
    }
    return 0;
}

/* Every second from the main loop: nothing here waits */
void plugin_mgr_tick(void) {
    reap_leftovers(0);
    /* /tmp is RAM: a log over the limit starts again (plugins write it with O_APPEND). At the
     * 10 s watchdog tick a plugin writing 2 MB/s reached 19 MB first. Every log of the run
     * folder, not only those of running slots: a helper writes on while its leader is dead
     * and waits for the next backoff restart. */
    DIR *d = opendir(HARMONY_PLUGINS_RUN_DIR);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        size_t l = strlen(de->d_name);
        if (l < 5 || strcmp(de->d_name + l - 4, ".log") != 0) continue;
        char log_path[400];
        struct stat lst;
        snprintf(log_path, sizeof(log_path), "%s/%s", HARMONY_PLUGINS_RUN_DIR, de->d_name);
        if (stat(log_path, &lst) == 0 && S_ISREG(lst.st_mode) && lst.st_size > PLUGIN_LOG_MAX_BYTES && truncate(log_path, 0) == 0) {
            printf("[*] Plugin log %s over %d KB: emptied\n", de->d_name, PLUGIN_LOG_MAX_BYTES / 1024);
        }
    }
    closedir(d);
}

void plugin_mgr_watchdog_tick(void) {
    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return;

    time_t now = time(NULL);
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;
        if (!is_plugin_enabled(de->d_name) || is_stopped_by_request(de->d_name)) continue;
        int slot = wd_slot(de->d_name);
        if (plugin_mgr_is_running(de->d_name)) {
            /* ran a whole watchdog period: no longer counted as crashing */
            if (slot >= 0) s_wd[slot].restarts = 0;
            continue;
        }
        if (slot >= 0) {
            if (now < s_wd[slot].next_try) continue;
            int n = s_wd[slot].restarts < 5 ? s_wd[slot].restarts : 5;
            int delay = 10 << (n + 1);
            if (delay > WD_MAX_DELAY_S) delay = WD_MAX_DELAY_S;
            s_wd[slot].restarts++;
            s_wd[slot].next_try = now + delay;
            printf("[!] Plugin '%s' is not running: restart %d (next one not before %d s)\n",
                   de->d_name, s_wd[slot].restarts, delay);
        }
        plugin_mgr_start(de->d_name);
    }
    closedir(d);
}

cJSON *plugin_mgr_get_installed_json(void) {
    cJSON *arr = cJSON_CreateArray();
    if (!arr) return NULL;

    DIR *d = opendir(HARMONY_PLUGINS_DIR);
    if (!d) return arr;

    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (de->d_name[0] == '.') continue;
        if (!is_safe_plugin_id(de->d_name)) continue;

        char mpath[256];
        snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", HARMONY_PLUGINS_DIR, de->d_name);
        size_t flen = 0;
        char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
        if (!mtxt) continue;

        cJSON *manifest = cJSON_Parse(mtxt);
        free(mtxt);
        if (!manifest) continue;

        cJSON *p_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(p_obj, "id", de->d_name);

        cJSON *j_name = cJSON_GetObjectItemCaseSensitive(manifest, "name");
        cJSON_AddStringToObject(p_obj, "name", (j_name && cJSON_IsString(j_name)) ? j_name->valuestring : de->d_name);

        cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(manifest, "version");
        cJSON_AddStringToObject(p_obj, "version", (j_ver && cJSON_IsString(j_ver)) ? j_ver->valuestring : "1.0.0");

        cJSON *j_desc = cJSON_GetObjectItemCaseSensitive(manifest, "description");
        if (j_desc && cJSON_IsString(j_desc)) cJSON_AddStringToObject(p_obj, "description", j_desc->valuestring);

        int enabled = is_plugin_enabled(de->d_name);
        cJSON_AddBoolToObject(p_obj, "enabled", enabled);

        pid_t pid = running_pid(de->d_name);
        int running = (pid > 0);
        cJSON_AddBoolToObject(p_obj, "running", running);
        cJSON_AddBoolToObject(p_obj, "stopped", is_stopped_by_request(de->d_name));
        cJSON_AddNumberToObject(p_obj, "restarts", wd_restarts(de->d_name));
        if (running) cJSON_AddNumberToObject(p_obj, "pid", (double)pid);

        cJSON *dev_def = cJSON_GetObjectItemCaseSensitive(manifest, "device_definition");
        if (dev_def) {
            cJSON_AddItemReferenceToObject(p_obj, "device_definition", dev_def);
        }
        cJSON_AddItemToObject(p_obj, "manifest", manifest);

        char plug_dir[256];
        snprintf(plug_dir, sizeof(plug_dir), "%s/%s", HARMONY_PLUGINS_DIR, de->d_name);
        uint64_t sz_b = compute_dir_size_bytes(plug_dir);
        cJSON_AddNumberToObject(p_obj, "size_bytes", (double)sz_b);
        char sz_fmt[32];
        format_bytes(sz_b, sz_fmt, sizeof(sz_fmt));
        cJSON_AddStringToObject(p_obj, "size_formatted", sz_fmt);

        cJSON_AddItemToArray(arr, p_obj);
    }
    closedir(d);
    return arr;
}

/* What the Hub needs from a manifest before it installs it */
static int validate_manifest(const cJSON *m, const char *src_dir, char *err_msg, size_t err_msg_sz) {
    const cJSON *j_id = cJSON_GetObjectItemCaseSensitive(m, "id");
    if (!cJSON_IsObject(m) || !cJSON_IsString(j_id) || !is_safe_plugin_id(j_id->valuestring)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest missing valid alphanumeric plugin id");
        return -1;
    }
    const cJSON *j_ver = cJSON_GetObjectItemCaseSensitive(m, "version");
    if (!cJSON_IsString(j_ver) || !j_ver->valuestring[0]) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest missing version");
        return -1;
    }
    const cJSON *j_ep = cJSON_GetObjectItemCaseSensitive(m, "entrypoint");
    if (!cJSON_IsString(j_ep) || !is_safe_rel_path(j_ep->valuestring)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest missing a valid entrypoint (relative path)");
        return -1;
    }
    if (src_dir) {
        char ep_path[512];
        snprintf(ep_path, sizeof(ep_path), "%s/%s", src_dir, j_ep->valuestring);
        struct stat st;
        if (stat(ep_path, &st) != 0 || !S_ISREG(st.st_mode)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "Entrypoint '%.60s' is not in the plugin", j_ep->valuestring);
            return -1;
        }
    }
    /* Shared libraries the plugin links (e.g. "libbearssl.so.0", docs/PLUGINS.md): each must be
     * in HARMONY_PLUGIN_LIB_DIR, else the plugin would be installed and then fail to start */
    const cJSON *libs = cJSON_GetObjectItemCaseSensitive(m, "requires_libs");
    if (libs) {
        const cJSON *l = NULL;
        if (!cJSON_IsArray(libs)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest requires_libs must be an array");
            return -1;
        }
        cJSON_ArrayForEach(l, libs) {
            if (!cJSON_IsString(l) || !is_safe_rel_path(l->valuestring) || strchr(l->valuestring, '/')) {
                if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest requires_libs holds a bad library name");
                return -1;
            }
            char lib_path[512];
            snprintf(lib_path, sizeof(lib_path), "%s/%s", HARMONY_PLUGIN_LIB_DIR, l->valuestring);
            if (access(lib_path, R_OK) != 0) {
                if (err_msg) snprintf(err_msg, err_msg_sz, "The plugin needs %.60s, which this Hub does not have (update the Hub software)",
                                      l->valuestring);
                return -1;
            }
        }
    }
    const cJSON *dd = cJSON_GetObjectItemCaseSensitive(m, "device_definition");
    if (dd && !cJSON_IsObject(dd)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest device_definition must be an object");
        return -1;
    }
    const cJSON *cf = dd ? cJSON_GetObjectItemCaseSensitive(dd, "config_fields") : NULL;
    if (cf) {
        const cJSON *f = NULL;
        if (!cJSON_IsArray(cf)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest config_fields must be an array");
            return -1;
        }
        cJSON_ArrayForEach(f, cf) {
            const cJSON *k = cJSON_GetObjectItemCaseSensitive(f, "key");
            if (!cJSON_IsObject(f) || !cJSON_IsString(k) || !k->valuestring[0]) {
                if (err_msg) snprintf(err_msg, err_msg_sz, "Every config_fields entry needs a string key");
                return -1;
            }
        }
    }
    const cJSON *dc = dd ? cJSON_GetObjectItemCaseSensitive(dd, "default_commands") : NULL;
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, (cJSON_IsArray(dc) ? dc : NULL)) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(c, "range");
        if (r && !cmd_range_valid(r)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "A default_commands range must be [min, max] (integers, min <= max)");
            return -1;
        }
        /* the infrared alternative of a command (cmd_ir_alt.h) */
        const cJSON *ikc = cJSON_GetObjectItemCaseSensitive(c, "irKeyCode");
        if (ikc && !cmd_ir_keycode_ok(ikc)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "A default_commands irKeyCode must be a non-empty string");
            return -1;
        }
        const cJSON *iw = cJSON_GetObjectItemCaseSensitive(c, "irWhen");
        if (iw && !cmd_ir_when_ok(iw)) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "A default_commands irWhen must be a config key (letters, digits, _ or -, at most 63)");
            return -1;
        }
    }
    return 0;
}

static int check_sha256(const char *path, const char *want, char *err_msg, size_t err_msg_sz) {
    char got[65];
    if (strlen(want) != 64 || file_sha256_hex(path, got) != 0 || strcasecmp(got, want) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Archive SHA-256 does not match the published checksum");
        return -1;
    }
    return 0;
}

/* /tmp is RAM: an unpack that leaves less than this free has taken hub_rf and hub_daemon
 * down with it (OOM killer) */
#define TMP_RAM_MARGIN (2ULL * 1024 * 1024)

static int install_archive_unlocked(const char *archive_path, const char *sha256_hex,
                                    const char *source_url, const char *source_repo, int dev,
                                    char *err_msg, size_t err_msg_sz) {
    if (!archive_path || access(archive_path, R_OK) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Plugin archive not readable");
        return -1;
    }
    if (sha256_hex && sha256_hex[0] && check_sha256(archive_path, sha256_hex, err_msg, err_msg_sz) != 0) return -1;

    char tmp_unpack[128];
    tmp_name(tmp_unpack, sizeof(tmp_unpack), "/tmp/p_unpack", "");

    char list_path[160];
    snprintf(list_path, sizeof(list_path), "%s.list", tmp_unpack);
    uint64_t unpacked = 0;
    if (!archive_entries_safe(archive_path, list_path, &unpacked)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Archive unreadable, or has a link, absolute or '..' entry");
        return -1;
    }
    if (unpacked + TMP_RAM_MARGIN > tmp_free_bytes()) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Not enough free RAM in /tmp to unpack %llu KB",
                              (unsigned long long)(unpacked / 1024));
        return -1;
    }
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGINS_DIR, 0755); /* statvfs needs the folder */
    if (unpacked + PLUGIN_FLASH_RESERVE > free_bytes_at(HARMONY_PLUGINS_DIR)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Not enough flash space for %llu KB of plugin files",
                              (unsigned long long)(unpacked / 1024));
        return -1;
    }
    mkdir(tmp_unpack, 0755);

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "export PATH=/cache/bin:/data/harmony/bin:$PATH; "
             "tar -xzf \"%s\" -C \"%s\" 2>/dev/null",
             archive_path, tmp_unpack);
    if (system(cmd) != 0) {
        delete_dir_recursive(tmp_unpack);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to unpack tar.gz archive");
        return -1;
    }

    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", tmp_unpack);
    if (access(mpath, R_OK) != 0) {
        /* Check if subfolder has manifest */
        DIR *td = opendir(tmp_unpack);
        if (td) {
            struct dirent *tde;
            while ((tde = readdir(td)) != NULL) {
                if (tde->d_name[0] == '.') continue;
                char sub_m[256];
                snprintf(sub_m, sizeof(sub_m), "%s/%s/manifest.json", tmp_unpack, tde->d_name);
                if (access(sub_m, R_OK) == 0) {
                    snprintf(mpath, sizeof(mpath), "%s", sub_m);
                    break;
                }
            }
            closedir(td);
        }
    }

    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    cJSON *manifest = mtxt ? cJSON_Parse(mtxt) : NULL;
    free(mtxt);
    if (!manifest) {
        delete_dir_recursive(tmp_unpack);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Archive missing valid manifest.json");
        return -1;
    }

    char src_dir[256];
    snprintf(src_dir, sizeof(src_dir), "%s", mpath);
    char *last_slash = strrchr(src_dir, '/');
    if (last_slash) *last_slash = '\0';

    int rc = validate_manifest(manifest, src_dir, err_msg, err_msg_sz);
    if (rc == 0) {
        char plug_id[64];
        snprintf(plug_id, sizeof(plug_id), "%s", cJSON_GetObjectItemCaseSensitive(manifest, "id")->valuestring);
        rc = replace_plugin_dir(plug_id, src_dir, source_url, source_repo, dev, err_msg, err_msg_sz);
    }
    cJSON_Delete(manifest);
    delete_dir_recursive(tmp_unpack);
    return rc;
}

int plugin_mgr_install_archive(const char *archive_path, const char *sha256_hex, char *err_msg, size_t err_msg_sz) {
    if (pthread_mutex_trylock(&s_install_lock) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Another plugin install is running");
        return PLUGIN_ERR_BUSY;
    }
    int rc = install_archive_unlocked(archive_path, sha256_hex, NULL, NULL, sha256_hex && sha256_hex[0] ? 0 : 1,
                                      err_msg, err_msg_sz);
    pthread_mutex_unlock(&s_install_lock);
    return rc;
}

/* The marketplace entry for a download URL or a plugin id: its download URL, SHA-256 and
 * repository. repo: only an entry of that repository matches the id (two repositories may
 * list the same id: a pull took the first one). */
static int catalog_lookup(const char *url, const char *id, const char *repo,
                          char url_out[512], char sha_out[65], char repo_out[512]) {
    sha_out[0] = '\0';
    if (url_out) url_out[0] = '\0';
    if (repo_out) repo_out[0] = '\0';
    cJSON *cat = plugin_mgr_get_marketplace_catalog(0);
    const cJSON *p = NULL;
    cJSON_ArrayForEach(p, cat) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "download_url");
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(p, "sha256");
        const cJSON *i = cJSON_GetObjectItemCaseSensitive(p, "id");
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(p, "repository");
        const char *r_url = cJSON_IsString(r) ? r->valuestring : "";
        if (!cJSON_IsString(u) || !cJSON_IsString(h) || strlen(h->valuestring) != 64) continue;
        if ((url && strcmp(u->valuestring, url) == 0) ||
            (id && cJSON_IsString(i) && strcmp(i->valuestring, id) == 0 &&
             (!repo || !repo[0] || strcmp(r_url, repo) == 0))) {
            snprintf(sha_out, 65, "%s", h->valuestring);
            if (url_out) snprintf(url_out, 512, "%s", u->valuestring);
            if (repo_out) snprintf(repo_out, 512, "%s", r_url);
            break;
        }
    }
    cJSON_Delete(cat);
    return sha_out[0] ? 0 : -1;
}

static int install_from_url_unlocked(const char *url, const char *sha256_hex, char *err_msg, size_t err_msg_sz) {
    if (!is_safe_url(url)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL (http/https, plain characters only)");
        return -1;
    }

    /* The SHA-256 (from the caller or the marketplace entry) is what tells a damaged or
     * replaced archive (the busybox wget fallback checks no certificate). It is required. */
    char want[65] = {0}, cat_sha[65], repo[512];
    /* its repository is recorded (for a pull) only when that entry has this checksum */
    int listed = catalog_lookup(url, NULL, NULL, NULL, cat_sha, repo) == 0;
    if (sha256_hex && sha256_hex[0]) snprintf(want, sizeof(want), "%s", sha256_hex);
    else if (listed) snprintf(want, sizeof(want), "%s", cat_sha);
    if (!listed || strcasecmp(want, cat_sha) != 0) repo[0] = '\0';
    if (!want[0]) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "No SHA-256 for this archive: pass sha256, or install it from a repository that lists one");
        return -1;
    }

    char tmp_tar[128];
    tmp_name(tmp_tar, sizeof(tmp_tar), "/tmp/plug_dl", ".tar.gz");

    int rc = fetch_url(url, tmp_tar, FETCH_ARCHIVE_MAX_BYTES, FETCH_ARCHIVE_MAX_MS);
    if (rc != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, rc == RUN_TOO_BIG ? "Archive larger than 1 MB" :
                                                   rc == RUN_TOO_SLOW ? "Archive download took too long" :
                                                   "Failed to download archive from URL");
        return rc == RUN_TOO_BIG ? -1 : PLUGIN_ERR_DOWNLOAD;
    }

    int res = install_archive_unlocked(tmp_tar, want, url, repo, 0, err_msg, err_msg_sz);
    unlink(tmp_tar);
    return res;
}

int plugin_mgr_install_from_url(const char *url, const char *sha256_hex, char *err_msg, size_t err_msg_sz) {
    if (pthread_mutex_trylock(&s_install_lock) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Another plugin install is running");
        return PLUGIN_ERR_BUSY;
    }
    int rc = install_from_url_unlocked(url, sha256_hex, err_msg, err_msg_sz);
    pthread_mutex_unlock(&s_install_lock);
    return rc;
}

static int install_dev_raw_unlocked(const char *url, char *err_msg, size_t err_msg_sz) {
    if (!is_safe_url(url)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid URL (http/https, plain characters only)");
        return -1;
    }

    char manifest_url[512] = {0};
    char base_url[512] = {0};

    size_t ulen = strlen(url);
    if (ulen > sizeof(manifest_url) - 32) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "URL too long");
        return -1;
    }

    if (strstr(url, "manifest.json") != NULL) {
        snprintf(manifest_url, sizeof(manifest_url), "%s", url);
        snprintf(base_url, sizeof(base_url), "%s", url);
        char *slash = strrchr(base_url, '/');
        if (slash) *slash = '\0';
    } else {
        if (url[ulen - 1] == '/') {
            snprintf(manifest_url, sizeof(manifest_url), "%smanifest.json", url);
            snprintf(base_url, sizeof(base_url), "%s", url);
            base_url[ulen - 1] = '\0';
        } else {
            snprintf(manifest_url, sizeof(manifest_url), "%s/manifest.json", url);
            snprintf(base_url, sizeof(base_url), "%s", url);
        }
    }

    char tmp_dev[128];
    tmp_name(tmp_dev, sizeof(tmp_dev), "/tmp/p_dev", "");
    mkdir(tmp_dev, 0755);

    /* 1. Download manifest.json */
    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", tmp_dev);
    int rc = fetch_url(manifest_url, mpath, FETCH_SMALL_MAX_BYTES, FETCH_SMALL_MAX_MS);
    if (rc != 0 || access(mpath, R_OK) != 0) {
        delete_dir_recursive(tmp_dev);
        if (err_msg) snprintf(err_msg, err_msg_sz, "Failed to download manifest.json from URL");
        return -1;
    }

    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    cJSON *manifest = mtxt ? cJSON_Parse(mtxt) : NULL;
    free(mtxt);
    if (!manifest || validate_manifest(manifest, NULL, err_msg, err_msg_sz) != 0) {
        if (!manifest && err_msg) snprintf(err_msg, err_msg_sz, "Invalid manifest.json syntax");
        cJSON_Delete(manifest);
        delete_dir_recursive(tmp_dev);
        return -1;
    }

    char plug_id[64] = {0};
    snprintf(plug_id, sizeof(plug_id), "%s", cJSON_GetObjectItemCaseSensitive(manifest, "id")->valuestring);
    const char *entry = cJSON_GetObjectItemCaseSensitive(manifest, "entrypoint")->valuestring;

    /* 2. Download the files the manifest lists, or the entrypoint alone */
    cJSON *j_files = cJSON_GetObjectItemCaseSensitive(manifest, "files");
    if (!j_files || !cJSON_IsArray(j_files)) {
        j_files = cJSON_GetObjectItemCaseSensitive(manifest, "dev_files");
    }
    cJSON *only_entry = NULL;
    if (!j_files || !cJSON_IsArray(j_files) || cJSON_GetArraySize(j_files) == 0) {
        only_entry = cJSON_CreateArray();
        cJSON_AddItemToArray(only_entry, cJSON_CreateString(entry));
        j_files = only_entry;
    }

    /* A file that is unsafe to name or fails to download stops the install: installed
     * without it, the plugin was left broken */
    const char *bad_file = NULL;
    uint64_t total = 0;
    cJSON *f_item = NULL;
    cJSON_ArrayForEach(f_item, j_files) {
        if (!f_item || !cJSON_IsString(f_item) || !f_item->valuestring[0]) continue;
        const char *fname = f_item->valuestring;
        if (strcmp(fname, "manifest.json") == 0) continue;
        if (!is_safe_rel_path(fname)) {
            bad_file = fname;
            break;
        }

        char local_path[384];
        snprintf(local_path, sizeof(local_path), "%s/%s", tmp_dev, fname);
        /* the parent directories, without a shell */
        for (char *c = local_path + strlen(tmp_dev) + 1; *c; c++) {
            if (*c == '/') {
                *c = '\0';
                mkdir(local_path, 0755);
                *c = '/';
            }
        }

        char file_url[768];
        snprintf(file_url, sizeof(file_url), "%s/%s", base_url, fname);
        if (fetch_url(file_url, local_path, FETCH_FILE_MAX_BYTES, FETCH_ARCHIVE_MAX_MS) != 0) {
            bad_file = fname;
            break;
        }
        struct stat st;
        if (stat(local_path, &st) == 0) total += (uint64_t)st.st_size;
        /* the downloads sit in /tmp (RAM) until the copy */
        if (total > FETCH_ARCHIVE_MAX_BYTES) {
            bad_file = fname;
            break;
        }
    }

    if (bad_file) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Manifest file '%.60s' is unsafe, too large or failed to download", bad_file);
        cJSON_Delete(only_entry);
        cJSON_Delete(manifest);
        delete_dir_recursive(tmp_dev);
        return -1;
    }
    cJSON_Delete(only_entry);
    /* the entrypoint must be among the downloaded files */
    rc = validate_manifest(manifest, tmp_dev, err_msg, err_msg_sz);
    cJSON_Delete(manifest);
    if (rc != 0) {
        delete_dir_recursive(tmp_dev);
        return -1;
    }

    /* 3. Install into /data/harmony/plugins/<plug_id> */
    rc = replace_plugin_dir(plug_id, tmp_dev, url, NULL, 1, err_msg, err_msg_sz);
    delete_dir_recursive(tmp_dev);
    return rc;
}

int plugin_mgr_install_dev_raw(const char *url, char *err_msg, size_t err_msg_sz) {
    if (pthread_mutex_trylock(&s_install_lock) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Another plugin install is running");
        return PLUGIN_ERR_BUSY;
    }
    int rc = install_dev_raw_unlocked(url, err_msg, err_msg_sz);
    pthread_mutex_unlock(&s_install_lock);
    return rc;
}

/* Updates an installed plugin from where it came from (.source.json): the same dev URL again,
 * or the current marketplace entry of its id, checked against that entry's SHA-256 */
int plugin_mgr_pull(const char *plugin_id, char *err_msg, size_t err_msg_sz) {
    if (!plugin_id || !is_safe_plugin_id(plugin_id)) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Invalid plugin ID");
        return -1;
    }
    char src_path[300];
    snprintf(src_path, sizeof(src_path), "%s/%s/.source.json", HARMONY_PLUGINS_DIR, plugin_id);
    size_t flen = 0;
    char *txt = read_file_text_alloc(src_path, 4096, &flen);
    cJSON *src = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    const cJSON *j_url = cJSON_GetObjectItemCaseSensitive(src, "url");
    int dev = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(src, "dev"));
    const cJSON *j_repo = cJSON_GetObjectItemCaseSensitive(src, "repository");
    char url[512] = {0}, repo[512] = {0};
    if (cJSON_IsString(j_url)) snprintf(url, sizeof(url), "%s", j_url->valuestring);
    if (cJSON_IsString(j_repo)) snprintf(repo, sizeof(repo), "%s", j_repo->valuestring);
    cJSON_Delete(src);

    if (pthread_mutex_trylock(&s_install_lock) != 0) {
        if (err_msg) snprintf(err_msg, err_msg_sz, "Another plugin install is running");
        return PLUGIN_ERR_BUSY;
    }
    int rc;
    char cat_url[512], sha[65];
    if (dev && url[0]) {
        rc = install_dev_raw_unlocked(url, err_msg, err_msg_sz);
    } else if (!repo[0] && (!url[0] || catalog_lookup(url, NULL, NULL, NULL, sha, repo) != 0 || !repo[0])) {
        /* an upload, or an install that recorded no repository and whose archive no catalog
         * lists now: the first entry with this id could come from any repository */
        if (err_msg) snprintf(err_msg, err_msg_sz, "Where '%s' came from is not known (uploaded, or installed by an older version): install it again from the marketplace", plugin_id);
        rc = -1;
    } else {
        /* from the repository it was installed from */
        if (catalog_lookup(NULL, plugin_id, repo, cat_url, sha, NULL) != 0) {
            if (err_msg) snprintf(err_msg, err_msg_sz, "Its repository no longer lists '%s'", plugin_id);
            rc = -1;
        } else {
            rc = install_from_url_unlocked(cat_url, sha, err_msg, err_msg_sz);
        }
    }
    pthread_mutex_unlock(&s_install_lock);
    return rc;
}

cJSON *plugin_mgr_get_device_definition(const char *plugin_id) {
    return plugin_mgr_get_manifest_item(plugin_id, "device_definition");
}

cJSON *plugin_mgr_get_manifest_item(const char *plugin_id, const char *key) {
    if (!plugin_id || !is_safe_plugin_id(plugin_id) || !key) return NULL;

    char mpath[256];
    snprintf(mpath, sizeof(mpath), "%s/%s/manifest.json", HARMONY_PLUGINS_DIR, plugin_id);
    size_t flen = 0;
    char *mtxt = read_file_text_alloc(mpath, 32768, &flen);
    if (!mtxt) return NULL;

    cJSON *manifest = cJSON_Parse(mtxt);
    free(mtxt);
    if (!manifest) return NULL;

    cJSON *item = cJSON_GetObjectItemCaseSensitive(manifest, key);
    cJSON *copy = item ? cJSON_Duplicate(item, 1) : NULL;
    cJSON_Delete(manifest);
    return copy;
}

int plugin_mgr_is_in_use_by_device(const char *plugin_id, char *device_name_out, size_t dev_name_sz) {
    if (!plugin_id || !plugin_id[0]) return 0;
    if (device_name_out && dev_name_sz > 0) device_name_out[0] = '\0';

    cJSON *root = config_store_devices_copy();
    if (!root) return 0;

    int in_use = 0;
    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (devs && cJSON_IsArray(devs)) {
        cJSON *dev = NULL;
        cJSON_ArrayForEach(dev, devs) {

            char res_id[64] = {0};
            int match = plugin_mgr_device_plugin_id(dev, res_id, sizeof(res_id)) && strcmp(res_id, plugin_id) == 0;

            if (!match) {
                cJSON *cmds = cJSON_GetObjectItemCaseSensitive(dev, "commands");
                if (cmds && cJSON_IsArray(cmds)) {
                    cJSON *cmd = NULL;
                    cJSON_ArrayForEach(cmd, cmds) {
                        cJSON *raw = cJSON_GetObjectItemCaseSensitive(cmd, "raw");
                        cJSON *kc = cJSON_GetObjectItemCaseSensitive(cmd, "keyCode");
                        const char *r_str = (raw && cJSON_IsString(raw)) ? raw->valuestring : "";
                        const char *k_str = (kc && cJSON_IsString(kc)) ? kc->valuestring : "";

                        char prefix[128];
                        snprintf(prefix, sizeof(prefix), "PLUGIN:%s:", plugin_id);
                        if (strncasecmp(r_str, prefix, strlen(prefix)) == 0 ||
                            strncasecmp(k_str, prefix, strlen(prefix)) == 0) {
                            match = 1;
                            break;
                        }
                    }
                }
            }

            if (match) {
                in_use = 1;
                if (device_name_out && dev_name_sz > 0) {
                    cJSON *j_name = cJSON_GetObjectItemCaseSensitive(dev, "name");
                    const char *n_str = (j_name && cJSON_IsString(j_name) && j_name->valuestring[0]) ?
                                        j_name->valuestring : NULL;
                    if (!n_str) {
                        cJSON *j_id = cJSON_GetObjectItemCaseSensitive(dev, "id");
                        n_str = (j_id && cJSON_IsString(j_id)) ? j_id->valuestring : "Device";
                    }
                    snprintf(device_name_out, dev_name_sz, "%s", n_str);
                }
                break;
            }
        }
    }

    cJSON_Delete(root);
    return in_use;
}

cJSON *plugin_mgr_get_storage_json(void) {
    cJSON *obj = cJSON_CreateObject();
    if (!obj) return NULL;

    struct statvfs vfs;
    const char *check_path = HARMONY_PLUGINS_DIR;
    if (statvfs(check_path, &vfs) != 0) {
        check_path = "/data";
        if (statvfs(check_path, &vfs) != 0) {
            check_path = "/";
            if (statvfs(check_path, &vfs) != 0) {
                memset(&vfs, 0, sizeof(vfs));
            }
        }
    }

    uint64_t total_bytes = (uint64_t)vfs.f_blocks * vfs.f_frsize;
    uint64_t free_bytes = (uint64_t)vfs.f_bavail * vfs.f_frsize;
    uint64_t used_bytes = (total_bytes >= free_bytes) ? (total_bytes - free_bytes) : 0;

    cJSON_AddNumberToObject(obj, "total_bytes", (double)total_bytes);
    cJSON_AddNumberToObject(obj, "free_bytes", (double)free_bytes);
    cJSON_AddNumberToObject(obj, "used_bytes", (double)used_bytes);

    char fmt[32];
    format_bytes(total_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "total_formatted", fmt);
    format_bytes(free_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "free_formatted", fmt);
    format_bytes(used_bytes, fmt, sizeof(fmt));
    cJSON_AddStringToObject(obj, "used_formatted", fmt);

    return obj;
}

cJSON *plugin_mgr_get_repositories(void) {
    size_t flen = 0;
    char *txt = read_file_text_alloc(HARMONY_REPOSITORIES_FILE, 32768, &flen);
    cJSON *root = txt ? cJSON_Parse(txt) : NULL;
    if (txt) free(txt);

    if (root && cJSON_IsArray(root)) return root;
    if (root) cJSON_Delete(root);

    cJSON *def = cJSON_CreateArray();
    cJSON_AddItemToArray(def, cJSON_CreateString(HARMONY_DEFAULT_REPO_URL));
    return def;
}

static int save_repositories(cJSON *arr) {
    if (!arr) return -1;
    char *out = cJSON_Print(arr);
    if (!out) return -1;
    mkdir(HARMONY_PLUGIN_ROOT, 0755);
    mkdir(HARMONY_PLUGIN_ROOT "/config", 0755);
    int ret = write_file_atomic_sized(HARMONY_REPOSITORIES_FILE, out, strlen(out));
    free(out);
    return ret;
}

int plugin_mgr_add_repository(const char *url) {
    if (!is_safe_url(url)) return -1;
    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, repos) {
        if (it->valuestring && strcmp(it->valuestring, url) == 0) {
            cJSON_Delete(repos);
            return 0; /* already present */
        }
    }
    cJSON_AddItemToArray(repos, cJSON_CreateString(url));
    int res = save_repositories(repos);
    cJSON_Delete(repos);
    return res;
}

int plugin_mgr_remove_repository(const char *url) {
    if (!url) return -1;
    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *new_repos = cJSON_CreateArray();
    cJSON *it = NULL;
    cJSON_ArrayForEach(it, repos) {
        if (it->valuestring && strcmp(it->valuestring, url) != 0) {
            cJSON_AddItemToArray(new_repos, cJSON_CreateString(it->valuestring));
        }
    }
    cJSON_Delete(repos);
    int res = save_repositories(new_repos);
    cJSON_Delete(new_repos);
    return res;
}

static cJSON *read_catalog_cache(const char *cache_path) {
    size_t flen = 0;
    char *cached = read_file_text_alloc(cache_path, FETCH_SMALL_MAX_BYTES * 4, &flen);
    if (!cached) return NULL;
    cJSON *c_root = cJSON_Parse(cached);
    free(cached);
    return c_root;
}

cJSON *plugin_mgr_get_marketplace_catalog(int force_reload) {
    const char *cache_path = HARMONY_PLUGINS_RUN_DIR "/marketplace_cache.json";
    struct stat st;
    if (!force_reload && stat(cache_path, &st) == 0 && (time(NULL) - st.st_mtime) < 600) {
        cJSON *c_root = read_catalog_cache(cache_path);
        if (c_root) return c_root;
    }

    cJSON *repos = plugin_mgr_get_repositories();
    cJSON *catalog = cJSON_CreateArray();
    int n_repos = cJSON_GetArraySize(repos);

    cJSON *repo_url = NULL;
    cJSON_ArrayForEach(repo_url, repos) {
        if (!repo_url->valuestring) continue;
        char tmp_repo[128];
        tmp_name(tmp_repo, sizeof(tmp_repo), HARMONY_PLUGINS_RUN_DIR "/repo_tmp", ".json");

        if (fetch_url(repo_url->valuestring, tmp_repo, FETCH_SMALL_MAX_BYTES, FETCH_SMALL_MAX_MS) == 0 &&
            access(tmp_repo, R_OK) == 0) {
            size_t rlen = 0;
            char *rtxt = read_file_text_alloc(tmp_repo, FETCH_SMALL_MAX_BYTES, &rlen);
            if (rtxt) {
                cJSON *rjson = cJSON_Parse(rtxt);
                free(rtxt);
                if (rjson) {
                    cJSON *plugs = cJSON_GetObjectItemCaseSensitive(rjson, "plugins");
                    if (plugs && cJSON_IsArray(plugs)) {
                        cJSON *p = NULL;
                        cJSON_ArrayForEach(p, plugs) {
                            /* a null or malformed entry broke the whole list in the UIs */
                            if (!cJSON_IsObject(p) || !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(p, "id"))) continue;
                            cJSON *e = cJSON_Duplicate(p, 1);
                            /* the repository it was read from, never one the entry names */
                            cJSON_DeleteItemFromObjectCaseSensitive(e, "repository");
                            cJSON_AddStringToObject(e, "repository", repo_url->valuestring);
                            cJSON_AddItemToArray(catalog, e);
                        }
                    }
                    cJSON_Delete(rjson);
                }
            }
        }
        unlink(tmp_repo);
    }
    cJSON_Delete(repos);

    /* Every repository failed (offline): the last catalog, even old, beats an empty one. With
     * no repository at all the catalog is empty. */
    if (cJSON_GetArraySize(catalog) == 0) {
        if (n_repos == 0) {
            unlink(cache_path);
            return catalog;
        }
        cJSON *stale = read_catalog_cache(cache_path);
        if (stale) {
            cJSON_Delete(catalog);
            return stale;
        }
        return catalog;
    }

    char *cat_str = cJSON_PrintUnformatted(catalog);
    if (cat_str) {
        mkdir(HARMONY_PLUGINS_RUN_DIR, 0755);
        int fd = open(cache_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd >= 0) {
            write(fd, cat_str, strlen(cat_str));
            close(fd);
        }
        free(cat_str);
    }

    return catalog;
}
