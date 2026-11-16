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
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
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

/* 1 = taken (release it), 0 = no lock file possible (run anyway), -1 = another test runs */
static int wifi_lock_take(void) {
    char pbuf[256];
    const char *path = wifi_config_path(WIFI_LOCK_PATH, pbuf, sizeof(pbuf));
    for (int attempt = 0; attempt < 2; attempt++) {
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0644);
        if (fd >= 0) {
            char pid[16];
            int n = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
            if (write(fd, pid, (size_t)n) != n) { /* an empty file still holds the lock */ }
            close(fd);
            return 1;
        }
        if (errno != EEXIST) return 0;
        long holder = 0;
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%ld", &holder) != 1) holder = 0;
            fclose(f);
        }
        /* held by this process (another thread of hub_daemon) or by a live one: busy. A file
         * whose holder is gone (killed in the middle of a test) is removed and tried again. */
        if (holder > 0 && (holder == (long)getpid() || kill((pid_t)holder, 0) == 0 || errno == EPERM)) return -1;
        unlink(path);
    }
    return -1;
}

static void wifi_lock_release(int taken) {
    if (taken != 1) return;
    char pbuf[256];
    unlink(wifi_config_path(WIFI_LOCK_PATH, pbuf, sizeof(pbuf)));
}

static int wifi_wait_seconds(void) {
#ifdef TEST_HARNESS
    const char *w = getenv("HARMONY_WIFI_TEST_WAIT");
    if (w && atoi(w) > 0) return atoi(w);
#endif
    return 12;
}

static void wifi_reconfigure_now(void) {
    char pbuf[256], cmd[400];
    snprintf(cmd, sizeof(cmd), "%s -i ath0 reconfigure >/dev/null 2>&1", wifi_config_path(WIFI_CLI_PATH, pbuf, sizeof(pbuf)));
    system(cmd);
}

static int wifi_test_run(const char *ssid, const char *psk, int is_open, int hidden, char *err_msg, size_t err_len) {
    char cli_buf[256], conf_buf[256], ath0_buf[256];
    const char *cli = wifi_config_path(WIFI_CLI_PATH, cli_buf, sizeof(cli_buf));
    const char *conf = wifi_config_path(WIFI_CONF_PATH, conf_buf, sizeof(conf_buf));
    const char *conf_ath0 = wifi_config_path(WIFI_CONF_ATH0_PATH, ath0_buf, sizeof(ath0_buf));
    char cmd[400];

    snprintf(cmd, sizeof(cmd), "%s -i ath0 ping 2>/dev/null", cli);
    FILE *f_ping = popen(cmd, "r");
    if (!f_ping) {
        return 0;
    }
    char ping_buf[32] = {0};
    if (fgets(ping_buf, sizeof(ping_buf), f_ping) == NULL || strstr(ping_buf, "PONG") == NULL) {
        pclose(f_ping);
#ifdef TEST_HARNESS
        if (!getenv("HARMONY_WIFI_ROOT")) return 0;
        if (err_msg && err_len) snprintf(err_msg, err_len, "wpa_supplicant is not running on ath0");
        return -1;
#else
        if (access(cli, X_OK) != 0) return 0;
        if (err_msg && err_len) snprintf(err_msg, err_len, "wpa_supplicant is not running on ath0");
        return -1;
#endif
    }
    pclose(f_ping);

    /* Back up current wpa_supplicant.conf */
    size_t bak_len = 0;
    char *orig_conf = read_file_text_alloc(conf, 16384, &bak_len);
    if (!orig_conf) {
        orig_conf = read_file_text_alloc(conf_ath0, 16384, &bak_len);
    }

    char test_wpa[1024];
    if (wifi_config_text(test_wpa, sizeof(test_wpa), ssid, psk, is_open, hidden) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "The Wi-Fi settings are too long");
        free(orig_conf);
        return -1;
    }

    if (write_file_content(conf, test_wpa) != 0 ||
        write_file_content(conf_ath0, test_wpa) != 0) {
        if (err_msg && err_len) snprintf(err_msg, err_len, "Could not write the Wi-Fi configuration (flash full?)");
        if (orig_conf) {
            write_file_content(conf, orig_conf);
            write_file_content(conf_ath0, orig_conf);
            free(orig_conf);
        }
        return -1;
    }

    wifi_reconfigure_now();

    int connected = 0;
    int wait_s = wifi_wait_seconds();
    snprintf(cmd, sizeof(cmd), "%s -i ath0 status 2>/dev/null", cli);
    for (int t = 0; t < wait_s; t++) {
        sleep(1);
        FILE *f_st = popen(cmd, "r");
        if (f_st) {
            char st_line[128];
            int is_completed = 0;
            int wrong_key = 0;
            while (fgets(st_line, sizeof(st_line), f_st)) {
                if (strstr(st_line, "wpa_state=COMPLETED")) is_completed = 1;
                if (strstr(st_line, "WRONG_KEY") || strstr(st_line, "auth_failures")) wrong_key = 1;
            }
            pclose(f_st);
            if (wrong_key) {
                if (err_msg && err_len) snprintf(err_msg, err_len, "Incorrect Wi-Fi password");
                break;
            }
            if (is_completed) {
                connected = 1;
                break;
            }
        }
    }

    if (!connected) {
        if (err_msg && err_len && !err_msg[0]) {
            snprintf(err_msg, err_len, "Connection timed out (AP unreachable or handshake failed)");
        }
        /* Restore previous configuration */
        if (orig_conf) {
            write_file_content(conf, orig_conf);
            write_file_content(conf_ath0, orig_conf);
            wifi_reconfigure_now();
            free(orig_conf);
        }
        return -1;
    }

    if (orig_conf) free(orig_conf);
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
