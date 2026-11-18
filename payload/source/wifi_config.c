/*
 * The Hub's Wi-Fi network: build, test, save (see wifi_config.h).
 *
 * Moved here from hub_network_util.c (the test) and http_api_network.c (the file text and the
 * save), so hub_daemon and hub_util use one implementation. Files go through
 * write_file_content() / read_file_text_alloc() (http_util.c) and atomic_file_write_pair()
 * (atomic_file.h).
 */
#include "wifi_config.h"
#include "wpa_conf.h"
#include "http_util.h"
#include "atomic_file.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

const char *wifi_config_path(const char *path, char *buf, size_t len) {
#ifdef TEST_HARNESS
    const char *root = getenv("HARMONY_WIFI_ROOT");
    if (root && root[0] && buf && len) {
        snprintf(buf, len, "%s%s", root, path);
        return buf;
    }
#endif
    (void)buf;
    (void)len;
    return path;
}

int wifi_config_text(char *buf, size_t len, const char *ssid, const char *psk, int is_open, int hidden) {
    if (!buf || !len || !ssid) return -1;
    int n;
    /* written as given: wpa_supplicant reads a quoted value without escapes (see wpa_conf.h) */
    if (is_open) {
        n = snprintf(buf, len,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n\n"
            "network={\n"
            "\tssid=\"%s\"\n"
            "\tkey_mgmt=NONE\n"
            "%s"
            "}\n",
            ssid, hidden ? "\tscan_ssid=1\n" : "");
    } else {
        const char *p = psk ? psk : "";
        n = snprintf(buf, len,
            "ctrl_interface=/var/run/wpa_supplicant\n"
            "update_config=1\n\n"
            "network={\n"
            "\tssid=\"%s\"\n"
            "\tpsk=%s%s%s\n"
            "%s"
            "}\n",
            ssid, wpa_psk_quote(p), p, wpa_psk_quote(p), hidden ? "\tscan_ssid=1\n" : "");
    }
    return (n < 0 || (size_t)n >= len) ? -1 : 0;
}

/* ---- one test at a time, across processes ---- */

/* The lock is a kernel lock (flock) on a file in /tmp, never the file itself: the kernel drops it
 * when the holder exits or is killed, so there is no stale holder to detect and no moment where
 * the file exists but its owner is unknown. Two threads of hub_daemon open the file separately,
 * so they exclude each other too. The file stays; only the lock comes and goes. */
static int g_wifi_lock_fd = -1;

/* 1 = taken (release it), 0 = no lock file possible (run anyway), -1 = another test runs */
static int wifi_lock_take(void) {
    char pbuf[256];
    const char *path = wifi_config_path(WIFI_LOCK_PATH, pbuf, sizeof(pbuf));
    int fd = open(path, O_CREAT | O_RDWR, 0644);
    if (fd < 0) return 0;
    /* close-on-exec: a helper started with system() during the test would keep the lock */
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        int busy = (errno == EWOULDBLOCK || errno == EAGAIN);
        close(fd);
        return busy ? -1 : 0;
    }
    g_wifi_lock_fd = fd;
    return 1;
}

static void wifi_lock_release(int taken) {
    if (taken != 1 || g_wifi_lock_fd < 0) return;
    int fd = g_wifi_lock_fd;
    g_wifi_lock_fd = -1; /* before the close: once it is closed another thread may take the lock */
    close(fd); /* drops the lock */
}

static int wifi_wait_seconds(void) {
#ifdef TEST_HARNESS
    const char *w = getenv("HARMONY_WIFI_TEST_WAIT");
    if (w && atoi(w) > 0) return atoi(w);
#endif
    return 12;
}

/* A shell command in a child that keeps none of the caller's descriptors (hub_daemon's
 * listening sockets must not stay open in a wpa_supplicant started here); waits for the shell
 * only, not for what it starts in the background. */
static void wifi_run_detached(const char *cmd) {
    pid_t pid = fork();
    if (pid == 0) {
        for (int fd = 3; fd < 1024; fd++) close(fd);
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    if (pid > 0) {
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {
        }
    }
}

static void wifi_cli(const char *args) {
    char pbuf[256], cmd[400];
    snprintf(cmd, sizeof(cmd), "%s -i ath0 %s >/dev/null 2>&1", wifi_config_path(WIFI_CLI_PATH, pbuf, sizeof(pbuf)), args);
    system(cmd);
}

/* wpa_supplicant answers on ath0's control socket */
static int wifi_supplicant_running(const char *cli) {
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "%s -i ath0 ping 2>/dev/null", cli);
    FILE *f = popen(cmd, "r");
    if (!f) return -1;
    char buf[32] = {0};
    int ok = fgets(buf, sizeof(buf), f) != NULL && strstr(buf, "PONG") != NULL;
    pclose(f);
    return ok;
}

/* Starts wpa_supplicant on ath0 with conf, the way rcS.local and network_manager.sh do (the
 * station interface is created first when it is missing) */
static void wifi_start_supplicant(const char *conf) {
    char wl[256], ifc[256], sup[256], cmd[1536];
    snprintf(cmd, sizeof(cmd),
             "%s ath0 >/dev/null 2>&1 || %s ath0 create wlandev wifi0 wlanmode sta >/dev/null 2>&1; "
             "%s ath0 up >/dev/null 2>&1; %s -s -iath0 -c %s >/dev/null 2>&1 &",
             wifi_config_path(WIFI_IFCONFIG_PATH, ifc, sizeof(ifc)),
             wifi_config_path(WIFI_WLANCONFIG_PATH, wl, sizeof(wl)),
             ifc, wifi_config_path(WIFI_SUPPLICANT_PATH, sup, sizeof(sup)), conf);
    wifi_run_detached(cmd);
}

/* ---- the previous files, kept on flash while a test runs ---- */

struct wifi_prev {
    char *conf;          /* NULL: the file did not exist */
    size_t conf_len;
    char *ath0;
    size_t ath0_len;
};

static void wifi_prev_free(struct wifi_prev *p) {
    free(p->conf);
    free(p->ath0);
    memset(p, 0, sizeof(*p));
}

/* One file: 0 with its text (NULL when it does not exist), -1 when it exists but cannot be
 * read (a test would lose it) */
static int wifi_prev_file(const char *path, char **text, size_t *len) {
    *text = read_file_text_alloc(path, 16384, len);
    if (*text || access(path, F_OK) != 0) return 0;
    return -1;
}

static int wifi_prev_read(struct wifi_prev *p, const char *conf, const char *ath0) {
    memset(p, 0, sizeof(*p));
    if (wifi_prev_file(conf, &p->conf, &p->conf_len) != 0 || wifi_prev_file(ath0, &p->ath0, &p->ath0_len) != 0) {
        wifi_prev_free(p);
        return -1;
    }
    return 0;
}

static void wifi_backup_remove(void) {
    char b1[256], b2[256], m[256];
    /* the mark first: without it the copies are never used */
    unlink(wifi_config_path(WIFI_TEST_MARK_PATH, m, sizeof(m)));
    unlink(wifi_config_path(WIFI_BACKUP_CONF_PATH, b1, sizeof(b1)));
    unlink(wifi_config_path(WIFI_BACKUP_ATH0_PATH, b2, sizeof(b2)));
}

/* copies first, then the mark; 0 or -1 (nothing left behind then) */
static int wifi_backup_write(const struct wifi_prev *p) {
    char b1[256], b2[256], m[256], mark[32];
    wifi_backup_remove();
    /* write_file_content: write, fsync, rename (atomic_file.h); the files are text */
    if (p->conf && write_file_content(wifi_config_path(WIFI_BACKUP_CONF_PATH, b1, sizeof(b1)), p->conf) != 0) goto fail;
    if (p->ath0 && write_file_content(wifi_config_path(WIFI_BACKUP_ATH0_PATH, b2, sizeof(b2)), p->ath0) != 0) goto fail;
    snprintf(mark, sizeof(mark), "conf=%d\nath0=%d\n", p->conf != NULL, p->ath0 != NULL);
    if (write_file_content(wifi_config_path(WIFI_TEST_MARK_PATH, m, sizeof(m)), mark) != 0) goto fail;
    return 0;
fail:
    wifi_backup_remove();
    return -1;
}

/* Each file back as it was (removed when it did not exist), wpa_supplicant pointed at it, then
 * the copies removed */
static void wifi_prev_restore(const struct wifi_prev *p, const char *conf, const char *ath0) {
    if (p->conf) (void)write_file_content(conf, p->conf);
    else unlink(conf);
    if (p->ath0) (void)write_file_content(ath0, p->ath0);
    else unlink(ath0);
    if (p->conf) {
        wifi_cli("reconfigure");
    } else {
        /* no file to read again: drop the candidate from the running wpa_supplicant */
        wifi_cli("remove_network all");
        wifi_cli("disconnect");
    }
    wifi_backup_remove();
}

/* with the lock held: an existing mark is from a test that did not finish */
static int wifi_recover_locked(void) {
    char m[256], b1[256], b2[256], c1[256], c2[256];
    const char *mark_path = wifi_config_path(WIFI_TEST_MARK_PATH, m, sizeof(m));
    size_t len = 0;
    char *mark = read_file_text_alloc(mark_path, 256, &len);
    if (!mark) return 0;
    int had_conf = strstr(mark, "conf=1") != NULL;
    int had_ath0 = strstr(mark, "ath0=1") != NULL;
    free(mark);
    struct wifi_prev p;
    memset(&p, 0, sizeof(p));
    if ((had_conf && !(p.conf = read_file_text_alloc(wifi_config_path(WIFI_BACKUP_CONF_PATH, b1, sizeof(b1)), 16384, &p.conf_len))) ||
        (had_ath0 && !(p.ath0 = read_file_text_alloc(wifi_config_path(WIFI_BACKUP_ATH0_PATH, b2, sizeof(b2)), 16384, &p.ath0_len)))) {
        printf("[wifi] An interrupted Wi-Fi test was found but its copies cannot be read; the Wi-Fi files stay as they are\n");
        fflush(stdout);
        wifi_prev_free(&p);
        wifi_backup_remove();
        return -1;
    }
    wifi_prev_restore(&p, wifi_config_path(WIFI_CONF_PATH, c1, sizeof(c1)),
                      wifi_config_path(WIFI_CONF_ATH0_PATH, c2, sizeof(c2)));
    wifi_prev_free(&p);
    printf("[wifi] An interrupted Wi-Fi test was undone: the previous Wi-Fi settings are back\n");
    fflush(stdout);
    return 1;
}

int wifi_config_recover(void) {
#ifdef TEST_HARNESS
    /* host builds without a test root never touch the host's /etc */
    if (!getenv("HARMONY_WIFI_ROOT")) return 0;
#endif
    int lock = wifi_lock_take();
    if (lock < 0) return 0; /* a live test: it restores the files itself */
    int rc = wifi_recover_locked();
    wifi_lock_release(lock);
    return rc;
}

static int wifi_test_run(const char *ssid, const char *psk, int is_open, int hidden, char *err_msg, size_t err_len) {
    char cli_buf[256], conf_buf[256], ath0_buf[256];
    const char *cli = wifi_config_path(WIFI_CLI_PATH, cli_buf, sizeof(cli_buf));
    const char *conf = wifi_config_path(WIFI_CONF_PATH, conf_buf, sizeof(conf_buf));
    const char *conf_ath0 = wifi_config_path(WIFI_CONF_ATH0_PATH, ath0_buf, sizeof(ath0_buf));
    char cmd[400];

#ifdef TEST_HARNESS
    /* host builds without a test root never touch the host's /etc */
    if (!getenv("HARMONY_WIFI_ROOT")) return 0;
#endif
    int running = wifi_supplicant_running(cli);
    if (running < 0) return 0;
#ifndef TEST_HARNESS
    if (running == 0 && access(cli, X_OK) != 0) return 0; /* no wpa_cli: not a Hub */
#endif
    /* a test cut short before this one: put its previous files back first */
    (void)wifi_recover_locked();

    char test_wpa[1024];
    if (wifi_config_text(test_wpa, sizeof(test_wpa), ssid, psk, is_open, hidden) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "The Wi-Fi settings are too long");
        return -1;
    }

    struct wifi_prev prev;
    if (wifi_prev_read(&prev, conf, conf_ath0) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "The current Wi-Fi configuration cannot be read; nothing was changed");
        return -1;
    }
    if (wifi_backup_write(&prev) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Could not write the Wi-Fi configuration (flash full?)");
        wifi_prev_free(&prev);
        return -1;
    }

    if (write_file_content(conf, test_wpa) != 0 ||
        write_file_content(conf_ath0, test_wpa) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Could not write the Wi-Fi configuration (flash full?)");
        wifi_prev_restore(&prev, conf, conf_ath0);
        wifi_prev_free(&prev);
        return -1;
    }

    /* A Hub with no Wi-Fi network stored has no wpa_supplicant (it cannot start without its
     * file): start it on the candidate, as the scripts would, instead of refusing */
    if (running > 0) {
        wifi_cli("reconfigure");
    } else {
        printf("[wifi] wpa_supplicant is not running on ath0: starting it for the test\n");
        fflush(stdout);
        wifi_start_supplicant(conf);
    }

    /* wpa_supplicant 0.7.2 (the Hub's) never reports a wrong key in "status": a wrong pass phrase
     * shows as the key handshake starting (4WAY_HANDSHAKE) and the state falling back to
     * SCANNING or DISCONNECTED, once every few seconds (seen on a Hub, 2026-11-17). Two such
     * drops end the test as a wrong password; a handshake seen but never completed at the end
     * counts too. A network that never gets past scanning is reported as not found. */
    int connected = 0;
    int wrong_key = 0;
    int saw_assoc = 0, in_handshake = 0, handshake_drops = 0;
    int wait_s = wifi_wait_seconds();
    snprintf(cmd, sizeof(cmd), "%s -i ath0 status 2>/dev/null", cli);
    for (int t = 0; t < wait_s && !connected && !wrong_key; t++) {
        sleep(1);
        FILE *f_st = popen(cmd, "r");
        if (!f_st) continue;
        char st_line[128];
        char state[32] = "";
        while (fgets(st_line, sizeof(st_line), f_st)) {
            if (strncmp(st_line, "wpa_state=", 10) == 0) {
                snprintf(state, sizeof(state), "%s", st_line + 10);
                state[strcspn(state, "\r\n")] = '\0';
            }
            if (strstr(st_line, "WRONG_KEY") || strstr(st_line, "auth_failures")) wrong_key = 1;
        }
        pclose(f_st);
        if (strcmp(state, "COMPLETED") == 0) {
            connected = 1;
        } else if (strcmp(state, "4WAY_HANDSHAKE") == 0 || strcmp(state, "GROUP_HANDSHAKE") == 0) {
            saw_assoc = 1;
            in_handshake = 1;
        } else if (strcmp(state, "ASSOCIATING") == 0 || strcmp(state, "ASSOCIATED") == 0 ||
                   strcmp(state, "AUTHENTICATING") == 0) {
            saw_assoc = 1;
        } else if (in_handshake && (strcmp(state, "SCANNING") == 0 || strcmp(state, "DISCONNECTED") == 0)) {
            in_handshake = 0;
            if (++handshake_drops >= 2) wrong_key = 1;
        }
    }
    if (!connected && !is_open && (handshake_drops > 0 || in_handshake)) wrong_key = 1;
    if (wrong_key) connected = 0;
    if (!connected && err_msg && err_len && !err_msg[0]) {
        if (wrong_key) snprintf(err_msg, err_len, "Incorrect Wi-Fi password");
        else if (!saw_assoc) snprintf(err_msg, err_len, "Wi-Fi network not found (check the name; 2.4 GHz networks only)");
    }

    if (!connected) {
        if (err_msg && err_len && !err_msg[0]) {
            snprintf(err_msg, err_len, "Connection timed out (AP unreachable or handshake failed)");
        }
        wifi_prev_restore(&prev, conf, conf_ath0);
        wifi_prev_free(&prev);
        return -1;
    }

    wifi_backup_remove();
    wifi_prev_free(&prev);
    return 0;
}

static int wifi_check_values(const char *ssid, const char *psk, int is_open, char *err_msg, size_t err_len) {
    if (!ssid || !ssid[0]) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "SSID cannot be empty");
        return -1;
    }
    const char *bad = wpa_conf_value_error(ssid, 0);
    if (!bad && !is_open) bad = wpa_conf_value_error(psk ? psk : "", 1);
    if (bad) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "%s", bad);
        return -1;
    }
    return 0;
}

int wifi_config_test(const char *ssid, const char *psk, int is_open, int hidden, char *err_msg, size_t err_len) {
    if (wifi_check_values(ssid, psk, is_open, err_msg, err_len) != 0) return -1;
    int lock = wifi_lock_take();
    if (lock < 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Another Wi-Fi test is running, try again in a few seconds");
        return -1;
    }
    int rc = wifi_test_run(ssid, psk, is_open, hidden, err_msg, err_len);
    wifi_lock_release(lock);
    return rc;
}

int wifi_config_save(const char *ssid, const char *psk, int is_open, int hidden, int apply) {
    char text[1024];
    if (wifi_config_text(text, sizeof(text), ssid, psk, is_open, hidden) != 0) return -1;
    char conf_buf[256], ath0_buf[256];
    /* both or neither: a failed second write puts the first file back */
    if (atomic_file_write_pair(wifi_config_path(WIFI_CONF_PATH, conf_buf, sizeof(conf_buf)),
                               wifi_config_path(WIFI_CONF_ATH0_PATH, ath0_buf, sizeof(ath0_buf)),
                               text, 16384) != 0) {
        return -1;
    }
    if (apply) {
        char pbuf[256], cmd[400];
        snprintf(cmd, sizeof(cmd), "%s -i ath0 reconfigure 2>/dev/null || killall -HUP wpa_supplicant 2>/dev/null &",
                 wifi_config_path(WIFI_CLI_PATH, pbuf, sizeof(pbuf)));
        system(cmd);
    }
    return 0;
}

int wifi_config_apply(const char *ssid, const char *psk, int is_open, int hidden, char *err_msg, size_t err_len) {
    if (wifi_check_values(ssid, psk, is_open, err_msg, err_len) != 0) return -1;
    int lock = wifi_lock_take();
    if (lock < 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Another Wi-Fi test is running, try again in a few seconds");
        return -1;
    }
    int rc = wifi_test_run(ssid, psk, is_open, hidden, err_msg, err_len);
    /* the test left wpa_supplicant on this very configuration: save it without another
     * reconfigure, which would drop the new link for a moment */
    if (rc == 0 && wifi_config_save(ssid, psk, is_open, hidden, 0) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Could not save the Wi-Fi settings (flash full?)");
        rc = -1;
    }
    wifi_lock_release(lock);
    return rc;
}

/* "key=value" line of wpa_cli status: the value when the line starts with key= */
static int status_value(const char *line, const char *key, char *out, size_t out_len) {
    size_t kl = strlen(key);
    if (strncmp(line, key, kl) != 0 || line[kl] != '=') return 0;
    snprintf(out, out_len, "%s", line + kl + 1);
    out[strcspn(out, "\r\n")] = '\0';
    return 1;
}

void wifi_config_status(struct wifi_status *st) {
    if (!st) return;
    memset(st, 0, sizeof(*st));
    char pbuf[256], cmd[400];
    snprintf(cmd, sizeof(cmd), "%s -i ath0 status 2>/dev/null", wifi_config_path(WIFI_CLI_PATH, pbuf, sizeof(pbuf)));
    FILE *f = popen(cmd, "r");
    if (f) {
        char line[160];
        while (fgets(line, sizeof(line), f)) {
            if (status_value(line, "wpa_state", st->wpa_state, sizeof(st->wpa_state))) continue;
            if (status_value(line, "ssid", st->ssid, sizeof(st->ssid))) continue;
            if (status_value(line, "key_mgmt", st->key_mgmt, sizeof(st->key_mgmt))) continue;
            status_value(line, "ip_address", st->ip, sizeof(st->ip));
        }
        pclose(f);
    }
    st->completed = strcmp(st->wpa_state, "COMPLETED") == 0;
    if (!st->completed) {
        /* the association data describes the network wpa_supplicant tries, not a link */
        st->ssid[0] = '\0';
        st->key_mgmt[0] = '\0';
    }
    if (!st->ip[0]) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s >= 0) {
            struct ifreq ifr;
            memset(&ifr, 0, sizeof(ifr));
            strncpy(ifr.ifr_name, "ath0", IFNAMSIZ - 1);
            ifr.ifr_addr.sa_family = AF_INET;
            if (ioctl(s, SIOCGIFADDR, &ifr) == 0) {
                inet_ntop(AF_INET, &((struct sockaddr_in *)&ifr.ifr_addr)->sin_addr, st->ip, sizeof(st->ip));
            }
            close(s);
        }
    }
    if (strcmp(st->ip, "0.0.0.0") == 0) st->ip[0] = '\0';
}

int wifi_usb_setup_setting(const char *conf_path) {
    char line[128];
    FILE *f = conf_path ? fopen(conf_path, "r") : NULL;
    if (!f) return 1;
    int on = 1;
    while (fgets(line, sizeof(line), f)) {
        char key[32], val[32];
        size_t k = 0, v = 0;
        const char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        while (*s && *s != '=' && *s != ' ' && *s != '\t' && k + 1 < sizeof(key)) key[k++] = *s++;
        key[k] = '\0';
        while (*s == ' ' || *s == '\t') s++;
        if (*s != '=' || strcmp(key, "USB_WIFI_SETUP") != 0) continue;
        for (s++; *s && v + 1 < sizeof(val); s++)
            if (*s != ' ' && *s != '\t' && *s != '"' && *s != '\r' && *s != '\n') val[v++] = *s;
        val[v] = '\0';
        on = strcmp(val, "0") != 0;
    }
    fclose(f);
    return on;
}
