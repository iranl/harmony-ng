#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <ctype.h>

#include "cJSON.h"
#include "codex_rf_proto.h"
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/file.h>
#include <linux/input.h>

#define RFSPI_DEV          "/dev/rfspi"
#define RFFW_DEV           "/dev/rffw"
#define CC2544_FW          "/lib/firmware/cc2544.bin"
#define DAEMON_PORT        8089
#define WEBUI_PORT         8089
#define RF_API_PORT        8092
#define ELITE_MAP_FILE     "/data/codex/elite_remote_map.json"
#define CURRENT_ACT_FILE   "/data/codex/current_activity"

static volatile bool g_running = true;
static int g_rf_fd = -1;
static pthread_mutex_t g_rf_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_paired_addr[4] = {0};
static uint8_t g_hub_rf_addr[4] = {0};
static uint8_t g_remote_slot = 0x01; /* Default to slot 1 for primary remote */
static bool g_has_paired_device = false;
static uint8_t g_fw_version = 0;
static volatile bool g_pairing_mode = false;
static void stop_pairing_mode(void);
static void start_pairing_mode(int slot);

#define MAX_PAIRED_REMOTES 2
typedef struct {
    uint8_t slot;
    uint8_t rf_addr[4];
    char name[16];
    bool active;
    bool link_lost;
    uint64_t last_seen_ms;
} paired_remote_t;

static uint64_t get_monotonic_ms(void);
static void set_hub_led(int state);
static uint8_t g_config_version = 1;

typedef struct {
    uint8_t percent;
    uint8_t charging;
    uint16_t voltage_mv;
    uint64_t last_update_ms;
} remote_battery_t;

static remote_battery_t g_remote_batteries[MAX_PAIRED_REMOTES] = {
    { .percent = 0, .charging = 0, .voltage_mv = 0, .last_update_ms = 0 },
    { .percent = 0, .charging = 0, .voltage_mv = 0, .last_update_ms = 0 }
};

typedef struct {
    char sw_version[32];
    uint8_t config_version;
    uint64_t last_report_ms;
} remote_version_t;

static remote_version_t g_remote_versions[MAX_PAIRED_REMOTES] = {
    { .sw_version = "unknown", .config_version = 0, .last_report_ms = 0 },
    { .sw_version = "unknown", .config_version = 0, .last_report_ms = 0 }
};

typedef struct {
    volatile bool in_progress;
    int target_slot;
    uint32_t total_bytes;
    uint32_t sent_bytes;
    uint16_t total_chunks;
    uint16_t sent_chunks;
    int status_code; /* 0=idle, 1=running, 2=complete, -1=error */
    char error_msg[64];
} rf_ota_state_t;

static rf_ota_state_t g_rf_ota = { false, 0, 0, 0, 0, 0, 0, "" };
static pthread_mutex_t g_ota_lock = PTHREAD_MUTEX_INITIALIZER;

static bool is_ota_busy(void) {
    pthread_mutex_lock(&g_ota_lock);
    bool b = g_rf_ota.in_progress;
    pthread_mutex_unlock(&g_ota_lock);
    return b;
}

static paired_remote_t g_paired_remotes[MAX_PAIRED_REMOTES] = {
    { .slot = 0x01, .active = false, .last_seen_ms = 0 },
    { .slot = 0x02, .active = false, .last_seen_ms = 0 }
};

static void mark_remote_slot_active(uint8_t slot) {
    if (slot == 0x01 || slot == 0x02) {
        int idx = slot - 1;
        g_paired_remotes[idx].active = true;
        g_paired_remotes[idx].link_lost = false;
        g_paired_remotes[idx].last_seen_ms = get_monotonic_ms();
        g_remote_slot = slot;
        g_has_paired_device = true;
    }
}

#define LOG_LEVEL_ERR   0
#define LOG_LEVEL_WARN  1
#define LOG_LEVEL_INFO  2
#define LOG_LEVEL_DEBUG 3

static int g_log_level = LOG_LEVEL_INFO;
#define RF_LOG(level, fmt, ...) do { if (g_log_level >= (level)) { printf(fmt, ##__VA_ARGS__); fflush(stdout); } } while (0)

static int g_nak_count = 0;
static int g_backoff_ms = 0;
#define NAK_BACKOFF_MAX_MS 5000
#define SYNC_DEDUP_MS      1000
static char g_last_sync_act[32] = "";
static uint64_t g_last_sync_ms = 0;
static int g_debug_logging = 0;

static uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void sig_handler(int sig) {
    (void)sig;
    g_running = false;
}

/* Flash CC2544 firmware if needed */
static int flash_cc2544(void) {
    int fin = open(CC2544_FW, O_RDONLY);
    if (fin < 0) return -1;
    int fout = open(RFFW_DEV, O_WRONLY);
    if (fout < 0) { close(fin); return -1; }

    char buf[1024];
    ssize_t n, total = 0;
    while ((n = read(fin, buf, sizeof(buf))) > 0) {
        if (write(fout, buf, n) != n) {
            close(fin);
            close(fout);
            return -1;
        }
        total += n;
    }
    close(fin);
    close(fout);
    printf("[+] Programmed %zd bytes into CC2544 firmware\n", total);
    usleep(100000);
    return 0;
}

/* Send raw HID++ command with mutex; returns 0 on success, -1 on failure */
static int rf_write_cmd(const uint8_t *cmd, size_t len) {
    if (g_rf_fd < 0) return -1;
    pthread_mutex_lock(&g_rf_mutex);
    ssize_t w = write(g_rf_fd, cmd, len);
    pthread_mutex_unlock(&g_rf_mutex);
    if (w < 0) {
        perror("write /dev/rfspi");
        return -1;
    }
    if (g_debug_logging) {
        printf("[*] TX (%zu bytes): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", cmd[i]);
        printf("\n");
    }
    return 0;
}

static uint8_t g_tx_seq = 0;
static uint8_t g_pending_ack_seq = 0;
static int g_ack_received = 0;
static int g_ack_status = 0;
static pthread_mutex_t g_ack_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ack_cond = PTHREAD_COND_INITIALIZER;

/* Send a Codex RF frame over /dev/rfspi using stock 30-byte Report 0x12 (Hub host/dongle framing matching hal_hub) */
static int rf_send_frame(rf_frame_t *frame) {
    uint8_t out[30];
    memset(out, 0, sizeof(out));
    out[0] = 0x12; /* Hub Report 0x12 (Host/Dongle mode) */
    uint8_t slot = frame->dev_slot ? (frame->dev_slot & 0x0F) : (g_remote_slot ? (g_remote_slot & 0x0F) : 0x01);
    out[1] = slot; /* Slot 0x01 or 0x02 */
    out[2] = frame->hdr_flag ? frame->hdr_flag : (0x50 | (frame->seq & 0x0F));
    out[3] = frame->msg_type;
    out[4] = frame->flags;
    memcpy(&out[5], frame->payload, 25);
    return rf_write_cmd(out, sizeof(out));
}

/* Send frame and wait for ACK (blocking, up to timeout_ms). Returns 0 on ACK, -1 on timeout. */
static int rf_send_frame_acked(rf_frame_t *frame, int timeout_ms) {
    pthread_mutex_lock(&g_ack_mutex);
    g_pending_ack_seq = frame->seq;
    g_ack_received = 0;
    pthread_mutex_unlock(&g_ack_mutex);

    if (rf_send_frame(frame) < 0) return -1;

    pthread_mutex_lock(&g_ack_mutex);
    if (!g_ack_received) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += timeout_ms / 1000;
        ts.tv_nsec += (timeout_ms % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_ack_cond, &g_ack_mutex, &ts);
    }
    int ok = g_ack_received;
    pthread_mutex_unlock(&g_ack_mutex);
    return ok ? 0 : -1;
}

/* Trigger HTTP POST request to local service (with connect timeout) */
static void http_post_local(int port, const char *endpoint, const char *json_body) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return;

    /* Non-blocking connect with 2s timeout */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval ctv = { .tv_sec = 0, .tv_usec = 150000 };
        if (select(sock + 1, NULL, &wfds, NULL, &ctv) <= 0) {
            close(sock);
            return;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(sock); return; }
    } else if (rc < 0) {
        close(sock);
        return;
    }

    /* Restore blocking mode with send/recv timeouts */
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 400000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    char req[1024];
    size_t blen = json_body ? strlen(json_body) : 0;
    int req_len = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n%s",
        endpoint, port, blen, json_body ? json_body : "");
    write(sock, req, req_len);
    char resp[256];
    read(sock, resp, sizeof(resp));
    close(sock);
}

/* Read current running activity ID from Hub */
static void get_current_activity(char *out, size_t outlen) {
    FILE *f = fopen(CURRENT_ACT_FILE, "r");
    if (f) {
        if (fgets(out, outlen, f)) {
            fclose(f);
            char *p = out + strlen(out) - 1;
            while (p >= out && (*p == '\r' || *p == '\n' || *p == ' ')) *p-- = '\0';
            if (out[0]) return;
        } else {
            fclose(f);
        }
    }
    snprintf(out, outlen, "-1");
}

/* In-memory cache for elite_remote_map.json */
static cJSON *g_elite_map_cache = NULL;
static time_t g_elite_map_mtime = 0;
static pthread_mutex_t g_map_lock = PTHREAD_MUTEX_INITIALIZER;

static cJSON *get_elite_map(void) {
    struct stat st;
    if (stat(ELITE_MAP_FILE, &st) != 0) return NULL;
    pthread_mutex_lock(&g_map_lock);
    if (st.st_mtime != g_elite_map_mtime || !g_elite_map_cache) {
        if (g_elite_map_cache) {
            cJSON_Delete(g_elite_map_cache);
            g_elite_map_cache = NULL;
        }
        FILE *mf = fopen(ELITE_MAP_FILE, "r");
        if (mf) {
            fseek(mf, 0, SEEK_END);
            long sz = ftell(mf);
            fseek(mf, 0, SEEK_SET);
            if (sz > 0 && sz < 500000) {
                char *buf = malloc(sz + 1);
                if (buf) {
                    size_t n = fread(buf, 1, sz, mf);
                    buf[n] = '\0';
                    g_elite_map_cache = cJSON_Parse(buf);
                    free(buf);
                }
            }
            fclose(mf);
        }
        g_elite_map_mtime = st.st_mtime;
        printf("[+] (Re)loaded elite remote map cache (size=%ld bytes)\n", (long)st.st_size);
    }
    pthread_mutex_unlock(&g_map_lock);
    return g_elite_map_cache;
}

/* Check if slot is in Direct Device Mode (2 Pages, no activities) */
static int is_slot_direct_mode(uint8_t slot) {
    if (slot < 1 || slot > 2) slot = 1;
    char path[64];
    snprintf(path, sizeof(path), "/data/codex/elite_slot%d_config.json", slot);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t rn = fread(buf, 1, sizeof(buf) - 1, f);
    buf[rn] = '\0';
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    if (!root) return 0;
    cJSON *rm = cJSON_GetObjectItem(root, "remoteMode");
    int direct = (rm && rm->valueint == 1) ? 1 : 0;
    cJSON_Delete(root);
    return direct;
}

static cJSON *get_elite_direct_map(int slot) {
    if (slot < 1 || slot > 2) slot = 1;
    char path[64];
    snprintf(path, sizeof(path), "/data/codex/elite_slot%d_direct_buttons.json", slot);
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 500000) { fclose(f); return NULL; }
    char *buf = malloc(sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, sz, f);
    buf[n] = '\0';
    fclose(f);
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    return root;
}

/* Dispatch button to configured action via cached elite_remote_map.json or direct map */
static void dispatch_elite_button(uint8_t slot, const char *button_name, const char *action_state) {
    int is_press = (strcmp(action_state, "release") != 0);

    /* Forward raw button event to daemon for MQTT (fire and forget) */
    char mqtt_body[256];
    snprintf(mqtt_body, sizeof(mqtt_body),
             "{\"command\":\"%s\",\"action\":\"%s\",\"source\":\"elite_rf\",\"slot\":%d}",
             button_name, action_state, slot);
    http_post_local(DAEMON_PORT, "/api/button-press", mqtt_body);

    if (!is_press) return;

    /* If remote is in Direct Device Mode, look up in elite_slotN_direct_buttons.json */
    if (is_slot_direct_mode(slot)) {
        cJSON *dmap = get_elite_direct_map(slot);
        if (dmap) {
            cJSON *btns = cJSON_GetObjectItem(dmap, "buttons");
            if (btns && cJSON_IsObject(btns)) {
                cJSON *b = cJSON_GetObjectItem(btns, button_name);
                if (b && cJSON_IsObject(b)) {
                    cJSON *jdev = cJSON_GetObjectItem(b, "deviceId");
                    if (!jdev) jdev = cJSON_GetObjectItem(b, "targetDevice");
                    cJSON *jcmd = cJSON_GetObjectItem(b, "command");
                    if (jdev && jcmd && jdev->valuestring && jcmd->valuestring) {
                        char body[256];
                        snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}",
                                 jdev->valuestring, jcmd->valuestring);
                        printf("[*] RF Dispatch Direct Button: %s -> dev=%s cmd=%s\n",
                               button_name, jdev->valuestring, jcmd->valuestring);
                        http_post_local(WEBUI_PORT, "/api/ir-send", body);
                    }
                }
            }
            cJSON_Delete(dmap);
        }
        return;
    }

    /* Standard Activity-Based mapping */
    cJSON *map = get_elite_map();
    if (!map) return;

    char act_id[32] = "-1";
    get_current_activity(act_id, sizeof(act_id));

    char act_type_buf[32] = {0};
    char dev_id_buf[64] = {0};
    char cmd_buf[64] = {0};
    char target_act_id_buf[64] = {0};

    pthread_mutex_lock(&g_map_lock);
    cJSON *acts = cJSON_GetObjectItemCaseSensitive(map, "activities");
    if (!acts || !cJSON_IsObject(acts)) acts = map;

    cJSON *btn_cfg = NULL;
    if (act_id[0] && strcmp(act_id, "-1") != 0) {
        cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(acts, act_id);
        if (act_obj && cJSON_IsObject(act_obj)) {
            btn_cfg = cJSON_GetObjectItemCaseSensitive(act_obj, button_name);
        }
    }
    if (!btn_cfg) {
        cJSON *def_obj = cJSON_GetObjectItemCaseSensitive(acts, "-1");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(acts, "default");
        if (!def_obj) def_obj = cJSON_GetObjectItemCaseSensitive(acts, "53591842");
        if (!def_obj && acts->child) def_obj = acts->child;
        if (def_obj && cJSON_IsObject(def_obj)) {
            btn_cfg = cJSON_GetObjectItemCaseSensitive(def_obj, button_name);
        }
    }

    if (btn_cfg && cJSON_IsObject(btn_cfg)) {
        cJSON *j_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "action");
        if (!j_act) j_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "type");
        if (j_act && cJSON_IsString(j_act)) strncpy(act_type_buf, j_act->valuestring, sizeof(act_type_buf) - 1);

        cJSON *j_dev = cJSON_GetObjectItemCaseSensitive(btn_cfg, "targetDevice");
        if (!j_dev) j_dev = cJSON_GetObjectItemCaseSensitive(btn_cfg, "deviceId");
        if (j_dev && cJSON_IsString(j_dev)) strncpy(dev_id_buf, j_dev->valuestring, sizeof(dev_id_buf) - 1);

        cJSON *j_cmd = cJSON_GetObjectItemCaseSensitive(btn_cfg, "command");
        if (j_cmd && cJSON_IsString(j_cmd)) strncpy(cmd_buf, j_cmd->valuestring, sizeof(cmd_buf) - 1);

        cJSON *j_target_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "activityId");
        if (!j_target_act) j_target_act = cJSON_GetObjectItemCaseSensitive(btn_cfg, "targetActivity");
        if (j_target_act && cJSON_IsString(j_target_act)) {
            strncpy(target_act_id_buf, j_target_act->valuestring, sizeof(target_act_id_buf) - 1);
        } else if (dev_id_buf[0]) {
            strncpy(target_act_id_buf, dev_id_buf, sizeof(target_act_id_buf) - 1);
        }
    }
    pthread_mutex_unlock(&g_map_lock);

    int is_cmd = (strcmp(act_type_buf, "device_cmd") == 0 || strcmp(act_type_buf, "command") == 0 ||
                  (act_type_buf[0] == '\0' && dev_id_buf[0] && cmd_buf[0]));
    if (is_cmd && dev_id_buf[0] && cmd_buf[0]) {
        char body[256];
        snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev_id_buf, cmd_buf);
        printf("[*] RF Dispatch: IR/BT send -> dev=%s cmd=%s\n", dev_id_buf, cmd_buf);
        http_post_local(WEBUI_PORT, "/api/ir-send", body);
    } else if ((strcmp(act_type_buf, "activity_start") == 0 || strcmp(act_type_buf, "change_activity") == 0) && target_act_id_buf[0]) {
        char body[64];
        snprintf(body, sizeof(body), "{\"activity\":\"%s\"}", target_act_id_buf);
        printf("[*] RF Dispatch: Start activity -> id=%s\n", target_act_id_buf);
        http_post_local(DAEMON_PORT, "/api/activity", body);
    } else if (strcmp(act_type_buf, "activity_stop") == 0 || strcmp(act_type_buf, "poweroff") == 0) {
        printf("[*] RF Dispatch: Stop activity (PowerOff)\n");
        http_post_local(DAEMON_PORT, "/api/poweroff", "{}");
    }
}

/* Asynchronous event dispatch queue to keep rx_worker completely non-blocking */
typedef enum {
    RF_EV_BUTTON = 0,
    RF_EV_DEVICE_CMD,
    RF_EV_ACTIVITY
} rf_ev_kind_t;

typedef struct {
    rf_ev_kind_t kind;
    uint8_t slot;
    char param1[64]; /* button_name OR dev_id OR act_id */
    char param2[64]; /* action_state OR command */
} rf_event_t;

#define RF_QUEUE_SIZE 64
static rf_event_t g_rf_queue[RF_QUEUE_SIZE];
static int g_rf_q_head = 0;
static int g_rf_q_tail = 0;
static pthread_mutex_t g_rf_q_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rf_q_cond = PTHREAD_COND_INITIALIZER;

static void enqueue_elite_button(uint8_t slot, const char *name, const char *action) {
    pthread_mutex_lock(&g_rf_q_mutex);
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_BUTTON;
        g_rf_queue[g_rf_q_head].slot = slot;
        strncpy(g_rf_queue[g_rf_q_head].param1, name ? name : "", sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        strncpy(g_rf_queue[g_rf_q_head].param2, action ? action : "", sizeof(g_rf_queue[0].param2) - 1);
        g_rf_queue[g_rf_q_head].param2[sizeof(g_rf_queue[0].param2) - 1] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

static void enqueue_elite_device_cmd(const char *dev_id, const char *cmd) {
    if (!dev_id || !cmd) return;
    pthread_mutex_lock(&g_rf_q_mutex);
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_DEVICE_CMD;
        g_rf_queue[g_rf_q_head].slot = 1;
        strncpy(g_rf_queue[g_rf_q_head].param1, dev_id, sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        strncpy(g_rf_queue[g_rf_q_head].param2, cmd, sizeof(g_rf_queue[0].param2) - 1);
        g_rf_queue[g_rf_q_head].param2[sizeof(g_rf_queue[0].param2) - 1] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow on dev_cmd!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

static void enqueue_elite_activity(const char *act_id) {
    if (!act_id) return;
    pthread_mutex_lock(&g_rf_q_mutex);
    int next = (g_rf_q_head + 1) % RF_QUEUE_SIZE;
    if (next != g_rf_q_tail) {
        g_rf_queue[g_rf_q_head].kind = RF_EV_ACTIVITY;
        g_rf_queue[g_rf_q_head].slot = 1;
        strncpy(g_rf_queue[g_rf_q_head].param1, act_id, sizeof(g_rf_queue[0].param1) - 1);
        g_rf_queue[g_rf_q_head].param1[sizeof(g_rf_queue[0].param1) - 1] = '\0';
        g_rf_queue[g_rf_q_head].param2[0] = '\0';
        g_rf_q_head = next;
        pthread_cond_signal(&g_rf_q_cond);
    } else {
        printf("[-] Warning: RF dispatch queue overflow on activity!\n");
    }
    pthread_mutex_unlock(&g_rf_q_mutex);
}

static void dispatch_activity_to_daemon(const char *act_id);

static void *dispatch_worker(void *arg) {
    (void)arg;
    while (g_running) {
        rf_event_t ev;
        bool has_event = false;

        pthread_mutex_lock(&g_rf_q_mutex);
        if (g_rf_q_head != g_rf_q_tail) {
            ev = g_rf_queue[g_rf_q_tail];
            g_rf_q_tail = (g_rf_q_tail + 1) % RF_QUEUE_SIZE;
            has_event = true;
        }
        pthread_mutex_unlock(&g_rf_q_mutex);

        if (!has_event) {
            usleep(20000); /* 20ms sleep */
            continue;
        }

        if (ev.kind == RF_EV_DEVICE_CMD) {
            char body[256];
            snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}",
                     ev.param1, ev.param2);
            http_post_local(WEBUI_PORT, "/api/ir-send", body);
        } else if (ev.kind == RF_EV_ACTIVITY) {
            dispatch_activity_to_daemon(ev.param1);
        } else {
            dispatch_elite_button(ev.slot, ev.param1, ev.param2);
        }
    }
    return NULL;
}

/* Dispatch activity start to daemon on 127.0.0.1:8089 */
static void dispatch_activity_to_daemon(const char *act_id) {
    if (strcmp(act_id, "-1") == 0) {
        printf("[*] RF Dispatching Activity Stop (PowerOff) to Hub\n");
        http_post_local(DAEMON_PORT, "/api/poweroff", "{}");
    } else {
        printf("[*] RF Dispatching Activity Start: %s to Hub\n", act_id);
        char body[64];
        snprintf(body, sizeof(body), "{\"activity\":\"%s\"}", act_id);
        http_post_local(DAEMON_PORT, "/api/activity", body);
    }
}

/* Dispatch device command from remote (e.g. Devices page on screen) to webui_server */
static void dispatch_device_command_to_webui(const char *payload) {
    char buf[128] = {0};
    strncpy(buf, payload, sizeof(buf) - 1);
    char *colon = strchr(buf, ':');
    if (!colon) return;
    *colon = '\0';
    const char *dev = buf;
    const char *cmd = colon + 1;
    char body[256];
    snprintf(body, sizeof(body), "{\"deviceId\":\"%s\",\"command\":\"%s\"}", dev, cmd);
    printf("[*] RF Dispatch Device Cmd: dev=%s cmd=%s\n", dev, cmd);
    http_post_local(WEBUI_PORT, "/api/ir-send", body);
}

/* Map RF button raw key code to human-readable Harmony name */
static const char *map_button_code(uint8_t k0, uint8_t k1) {
    uint16_t key = ((uint16_t)k0 << 8) | k1;
    switch (key) {
        /* Real Harmony RF keycodes */
        case 0x01EC: return "PowerOffActivity";
        case 0x0052: return "DirectionUp";
        case 0x0051: return "DirectionDown";
        case 0x0050: return "DirectionLeft";
        case 0x004F: return "DirectionRight";
        case 0x0058: return "Select";
        case 0x0225: return "Back";
        case 0x0065: return "Menu";
        case 0x0094: return "Exit";
        case 0x01FF: return "Info";
        case 0x008D: return "Guide";
        case 0x0089: return "Live";
        case 0x009A: return "Dvr";
        case 0x00E9: return "VolumeUp";
        case 0x00EA: return "VolumeDown";
        case 0x00E2: return "VolumeMute";
        case 0x009C: return "ChannelUp";
        case 0x009D: return "ChannelDown";
        case 0x0224: return "PrevChannel";
        case 0x00B0: return "Play";
        case 0x00B1: return "Pause";
        case 0x00B2: return "Record";
        case 0x00B3: return "FastForward";
        case 0x00B4: return "Rewind";
        case 0x00B7: return "Stop";
        case 0x01F7: return "Red";
        case 0x01F6: return "Green";
        case 0x01F5: return "Yellow";
        case 0x01F4: return "Blue";
        case 0x0FF2: return "Ha1";
        case 0x0FF3: return "Ha2";
        case 0x0FF4: return "Ha3";
        case 0x0FF5: return "Ha4";
        case 0x0FF0: return "RockerUp";
        case 0x0FF1: return "RockerDown";

        default: return NULL;
    }
}

/* Look up activity name by ID exclusively from /data/codex/config/activities.json */
static void get_activity_name_by_id(const char *act_id, char *out_name, size_t out_len) {
    if (!act_id || strcmp(act_id, "-1") == 0) {
        snprintf(out_name, out_len, "PowerOff");
        return;
    }
    FILE *f = fopen("/data/codex/config/activities.json", "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, f);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "Activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            cJSON *j_name = cJSON_GetObjectItem(elem, "name");
                            if (!j_name) j_name = cJSON_GetObjectItem(elem, "Name");
                            cJSON *j_id = cJSON_GetObjectItem(elem, "id");
                            if (!j_id) j_id = cJSON_GetObjectItem(elem, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(elem, "Id-");
                            if (j_name && cJSON_IsString(j_name) && j_id) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                if (strcmp(id_str, act_id) == 0) {
                                    strncpy(out_name, j_name->valuestring, out_len - 1);
                                    out_name[out_len - 1] = '\0';
                                    cJSON_Delete(root);
                                    fclose(f);
                                    return;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(f);
    }
    strncpy(out_name, act_id, out_len - 1);
    out_name[out_len - 1] = '\0';
}


/* Publish remote battery status to daemon / MQTT */
static void publish_remote_battery_mqtt(uint8_t slot, uint8_t pct, uint8_t chg, uint16_t mv) {
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return;
    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(DAEMON_PORT);
    sin.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(s, (struct sockaddr *)&sin, sizeof(sin)) != 0) {
        close(s);
        return;
    }

    char body[512];
    snprintf(body, sizeof(body),
        "{\"action\":\"mqtt_publish\",\"topic\":\"harmony/hub/remote/%d/battery\","
        "\"payload\":\"{\\\"battery\\\":%d,\\\"charging\\\":%s,\\\"voltage\\\":%.2f}\",\"retain\":1}",
        slot, pct, chg ? "true" : "false", (float)mv / 1000.0f);

    char req[768];
    int rlen = snprintf(req, sizeof(req),
        "POST /api/action HTTP/1.1\r\nHost: 127.0.0.1:%d\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n%s",
        DAEMON_PORT, (int)strlen(body), body);
    write(s, req, rlen);
    close(s);
}

static void record_config_diff(int slot, const char *old_json_str, cJSON *norm) {
    char diff[256] = {0};
    int diff_len = 0;
    cJSON *old_root = (old_json_str && *old_json_str) ? cJSON_Parse(old_json_str) : NULL;

    const char *keys[] = {
        "brightness", "timeout", "touch", "gyro", "nav_key", "nav_mode",
        "btn_backlight", "night_start", "night_end", "remote_mode", "usbeth", "no_screen",
        "cap_buttons"
    };
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        cJSON *nitem = cJSON_GetObjectItem(norm, keys[i]);
        if (!nitem) continue;
        int nval = nitem->valueint;
        int oval = -999;
        if (old_root) {
            cJSON *oitem = cJSON_GetObjectItem(old_root, keys[i]);
            if (oitem) oval = oitem->valueint;
        }
        if (oval != nval && oval != -999) {
            char chunk[64];
            snprintf(chunk, sizeof(chunk), "%s%s: %d->%d", diff_len ? ", " : "", keys[i], oval, nval);
            strncat(diff, chunk, sizeof(diff) - diff_len - 1);
            diff_len = strlen(diff);
        }
    }
    if (old_root) cJSON_Delete(old_root);

    if (diff_len == 0) {
        snprintf(diff, sizeof(diff), "Remote Slot %d settings modified", slot);
    }

    FILE *df = fopen("/data/codex/rf_sync_pending_details.json", "w");
    if (df) {
        fprintf(df, "{\"slot\":%d,\"diff\":\"%s\"}\n", slot, diff);
        fclose(df);
    }
    FILE *flag = fopen("/data/codex/rf_sync_pending.flag", "w");
    if (flag) {
        fprintf(flag, "%ld\n", (long)time(NULL));
        fclose(flag);
    }
}

/* Save remote config backup to /data/codex/elite_slotN_config.json */
static void save_remote_config_backup(uint8_t slot, const uint8_t *p) {
    if (slot < 1 || slot > 2) slot = 1;
    char path[64];
    snprintf(path, sizeof(path), "/data/codex/elite_slot%d_config.json", slot);
    cJSON *root = cJSON_CreateObject();
    if (!root) return;

    int timeout = (int)p[1] | ((int)p[16] << 8);
    if (timeout <= 0) timeout = (int)p[1];
    if (timeout <= 0) timeout = 120;

    int gyro = (p[11] != 0);
    int gyro_rate = p[12] > 0 ? p[12] : 25;
    int btn_backlight = p[13];
    int night_start = p[14];
    int night_end = p[15];

    cJSON_AddNumberToObject(root, "slot", slot);
    cJSON_AddNumberToObject(root, "brightness", p[0]);
    cJSON_AddNumberToObject(root, "timeout", timeout);
    cJSON_AddBoolToObject(root, "touchEnabled", p[2] != 0);
    cJSON_AddNumberToObject(root, "touch", p[2] != 0 ? 1 : 0);
    uint16_t nk = (uint16_t)p[3] | ((uint16_t)p[4] << 8);
    cJSON_AddNumberToObject(root, "navToggleKey", nk);
    cJSON_AddNumberToObject(root, "nav_key", nk);
    cJSON_AddNumberToObject(root, "navToggleMode", p[5]);
    cJSON_AddNumberToObject(root, "nav_mode", p[5]);
    cJSON_AddBoolToObject(root, "usbEth", p[6] != 0);
    cJSON_AddNumberToObject(root, "usbeth", p[6] != 0 ? 1 : 0);
    cJSON_AddBoolToObject(root, "confirmActivity", p[7] != 0);
    cJSON_AddNumberToObject(root, "confirm_act", p[7] != 0 ? 1 : 0);
    cJSON_AddNumberToObject(root, "version", p[8]);
    int ts = (p[9] >= 1 && p[9] <= 3) ? p[9] : 2;
    cJSON_AddNumberToObject(root, "touchSensitivity", ts);
    cJSON_AddNumberToObject(root, "touch_sensitivity", ts);
    int rm = p[10] == 1 ? 1 : 0;
    cJSON_AddNumberToObject(root, "remoteMode", rm);
    cJSON_AddNumberToObject(root, "remote_mode", rm);
    cJSON_AddBoolToObject(root, "gyroEnabled", gyro != 0);
    cJSON_AddNumberToObject(root, "gyro", gyro != 0 ? 1 : 0);
    cJSON_AddNumberToObject(root, "gyroRate", gyro_rate);
    cJSON_AddNumberToObject(root, "gyro_rate", gyro_rate);
    cJSON_AddNumberToObject(root, "btnBacklight", btn_backlight);
    cJSON_AddNumberToObject(root, "btn_backlight", btn_backlight);
    cJSON_AddNumberToObject(root, "nightStart", night_start);
    cJSON_AddNumberToObject(root, "night_start", night_start);
    cJSON_AddNumberToObject(root, "nightEnd", night_end);
    cJSON_AddNumberToObject(root, "night_end", night_end);
    int cap_buttons = (p[17] == 0) ? 0 : 1;
    cJSON_AddBoolToObject(root, "capButtonsEnabled", cap_buttons != 0);
    cJSON_AddNumberToObject(root, "cap_buttons", cap_buttons);

    char *txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (txt) {
        int need_write = 1;
        FILE *ef = fopen(path, "r");
        if (ef) {
            fseek(ef, 0, SEEK_END);
            long sz = ftell(ef);
            fseek(ef, 0, SEEK_SET);
            if (sz == (long)strlen(txt)) {
                char *cur = (char *)malloc((size_t)sz + 1);
                if (cur) {
                    if (fread(cur, 1, (size_t)sz, ef) == (size_t)sz) {
                        cur[sz] = '\0';
                        if (strcmp(cur, txt) == 0) {
                            need_write = 0;
                        }
                    }
                    free(cur);
                }
            }
            fclose(ef);
        }
        if (need_write) {
            FILE *f = fopen(path, "w");
            if (f) {
                fputs(txt, f);
                fclose(f);
                printf("[*] Saved remote config backup to %s\n", path);
            }
        }
        free(txt);
    }
}

/* Push remote display / navigation settings to a specific remote slot */
static void rf_send_settings_push_slot(uint8_t slot, int bright, int timeout, int touch, int gyro,
                                       int nav_key, int nav_mode, int confirm_act, int touch_sens,
                                       int remote_mode, int gyro_rate, int btn_backlight,
                                       int night_start, int night_end, int usbeth, int no_screen,
                                       int cap_buttons) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;
    rf_frame_t f;
    uint8_t payload[26] = {0};
    payload[0] = (uint8_t)(bright < 1 ? 1 : (bright > 5 ? 5 : bright));
    payload[1] = (uint8_t)(touch ? 1 : 0);
    payload[2] = (uint8_t)(gyro ? 1 : 0);
    payload[3] = (uint8_t)((timeout >> 8) & 0xFF);
    payload[4] = (uint8_t)(timeout & 0xFF);
    payload[5] = (uint8_t)(nav_key & 0xFF);
    payload[6] = (uint8_t)((nav_key >> 8) & 0xFF);
    payload[7] = (uint8_t)nav_mode;
    payload[8] = (uint8_t)(confirm_act ? 1 : 0);
    payload[9] = (uint8_t)(touch_sens >= 1 && touch_sens <= 3 ? touch_sens : 2);
    payload[10] = (uint8_t)(remote_mode == 1 ? 1 : 0);
    payload[11] = (uint8_t)(gyro_rate > 0 ? gyro_rate : 25);
    payload[12] = (uint8_t)(btn_backlight >= 0 && btn_backlight <= 2 ? btn_backlight : 1);
    payload[13] = (uint8_t)(night_start >= 0 && night_start <= 23 ? night_start : 22);
    payload[14] = (uint8_t)(night_end >= 0 && night_end <= 23 ? night_end : 7);
    payload[15] = (uint8_t)(usbeth ? 1 : 0);
    payload[16] = (uint8_t)g_config_version;
    payload[17] = (uint8_t)(no_screen ? 1 : 0);
    payload[18] = (uint8_t)(cap_buttons ? 1 : 2);
    rf_build_frame(&f, slot, RF_MSG_SETTINGS_PUSH, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 19);
    rf_send_frame_acked(&f, 300);
}

/* Send Codex Activity Sync frame to a specific slot */
static void rf_send_codex_activity_sync_slot(const char *act_id, uint8_t seq, uint8_t slot, uint8_t is_query_reply) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    char act_name[24] = {0};
    get_activity_name_by_id(act_id, act_name, sizeof(act_name));

    rf_frame_t f;
    uint8_t payload[25] = {0};
    if (strlen(act_id) <= 2 || strcmp(act_id, "-1") == 0) {
        strncpy((char *)&payload[0], act_id, 3);
        strncpy((char *)&payload[3], act_name, 21);
    } else {
        strncpy((char *)&payload[0], act_id, 10);
        strncpy((char *)&payload[10], act_name, 14);
    }
    payload[24] = is_query_reply; /* 1 = reply to SYNC_REQUEST (steady-state catchup), 0 = live transition */

    rf_build_frame(&f, slot ? slot : 0x01, RF_MSG_ACTIVITY_SYNC, seq, 0, payload, 25);
    rf_send_frame(&f);
}

/* Send Codex Activity Sync frame with explicit sequence number to active remotes */
static void rf_send_codex_activity_sync_seq(const char *act_id, uint8_t seq) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            rf_send_codex_activity_sync_slot(act_id, seq, g_paired_remotes[i].slot, 0x00);
            usleep(10000);
        }
    }
}

/* Send Codex Activity Sync frame (RF_MSG_ACTIVITY_SYNC) */
static void rf_send_codex_activity_sync(const char *act_id) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    /* Dedup */
    uint64_t now = get_monotonic_ms();
    if (strcmp(act_id, g_last_sync_act) == 0 && (now - g_last_sync_ms) < SYNC_DEDUP_MS) return;
    strncpy(g_last_sync_act, act_id, sizeof(g_last_sync_act) - 1);
    g_last_sync_act[sizeof(g_last_sync_act) - 1] = '\0';
    g_last_sync_ms = now;

    rf_send_codex_activity_sync_seq(act_id, ++g_tx_seq);
}

/* Send Codex Time & Timezone sync frame to a specific slot */
static void rf_send_codex_time_sync_slot(uint8_t slot) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    rf_frame_t f;
    uint8_t payload[26] = {0};
    uint32_t now_epoch = (uint32_t)time(NULL);

    /* 4 bytes epoch seconds (little-endian) */
    payload[0] = (uint8_t)(now_epoch & 0xFF);
    payload[1] = (uint8_t)((now_epoch >> 8) & 0xFF);
    payload[2] = (uint8_t)((now_epoch >> 16) & 0xFF);
    payload[3] = (uint8_t)((now_epoch >> 24) & 0xFF);

    /* Read /etc/TZ */
    FILE *ftz = fopen("/etc/TZ", "r");
    char tz[64] = "CET-1CEST,M3.5.0,M10.5.0/3";
    if (ftz) {
        if (fgets(tz, sizeof(tz), ftz)) {
            char *nl = strchr(tz, '\n');
            if (nl) *nl = '\0';
            char *cr = strchr(tz, '\r');
            if (cr) *cr = '\0';
        }
        fclose(ftz);
    }
    /* If string exceeds 15 bytes, truncate at first comma so we send a clean prefix instead of a malformed chunk */
    if (strlen(tz) > 15) {
        char *comma = strchr(tz, ',');
        if (comma) *comma = '\0';
    }
    uint8_t tz_len = (uint8_t)strlen(tz);
    if (tz_len > 15) tz_len = 15;
    payload[4] = tz_len;
    memcpy(&payload[5], tz, tz_len);

    /* Config version embedded at byte 20 */
    payload[20] = g_config_version;

    /* Embed Hub RF address in bytes 21..24 */
    if (g_hub_rf_addr[0] || g_hub_rf_addr[1] || g_hub_rf_addr[2] || g_hub_rf_addr[3]) {
        memcpy(&payload[21], g_hub_rf_addr, 4);
    } else if (g_paired_addr[0] || g_paired_addr[1] || g_paired_addr[2] || g_paired_addr[3]) {
        memcpy(&payload[21], g_paired_addr, 4);
    }

    rf_build_frame(&f, slot ? slot : 0x01, RF_MSG_TIME_SYNC, ++g_tx_seq, 0, payload, 26);
    rf_send_frame(&f);
}

/* Send Codex Time & Timezone sync frame to all active paired remotes */
static void rf_send_codex_time_sync(void) {
    if (g_rf_fd < 0 || !g_has_paired_device) return;

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            rf_send_codex_time_sync_slot(g_paired_remotes[i].slot);
            usleep(10000);
        }
    }
}

static uint8_t map_activity_icon_type(const char *icon, const char *name, const char *type) {
    char low[64] = {0};
    const char *src = (icon && icon[0]) ? icon : ((name && name[0]) ? name : (type ? type : ""));
    for (int k = 0; src[k] && k < 63; k++) low[k] = tolower((unsigned char)src[k]);

    if (strstr(low, "watchtv") || strstr(low, "tv") || strstr(low, "television")) return 1;
    if (strstr(low, "movie") || strstr(low, "cinema") || strstr(low, "stream") || strstr(low, "netflix") || strstr(low, "plex")) return 2;
    if (strstr(low, "music") || strstr(low, "audio") || strstr(low, "radio") || strstr(low, "spotify")) return 3;
    if (strstr(low, "game") || strstr(low, "console")) return 4;
    if (strstr(low, "ps") || strstr(low, "playstation")) return 5;
    if (strstr(low, "xbox")) return 6;
    if (strstr(low, "switch") || strstr(low, "nintendo")) return 7;
    if (strstr(low, "shield")) return 8;
    if (strstr(low, "apple")) return 9;
    if (strstr(low, "roku")) return 10;
    if (strstr(low, "firetv") || strstr(low, "amazon") || strstr(low, "fire")) return 11;
    if (strstr(low, "pc") || strstr(low, "computer") || strstr(low, "laptop")) return 12;
    if (strstr(low, "smarttv") || strstr(low, "smart")) return 13;
    if (strstr(low, "sonos")) return 14;
    if (strstr(low, "receiver") || strstr(low, "avr") || strstr(low, "amp") || strstr(low, "heos") || strstr(low, "denon")) return 15;
    if (strstr(low, "light") || strstr(low, "lamp") || strstr(low, "bulb")) return 16;
    if (strstr(low, "power") || strstr(low, "off")) return 17;
    return 1;
}

/* Background worker: pushes full config to Remote */
static void *config_push_worker(void *arg) {
    (void)arg;
    static pthread_mutex_t push_lock = PTHREAD_MUTEX_INITIALIZER;
    if (pthread_mutex_trylock(&push_lock) != 0) {
        printf("[-] Config push already in progress\n");
        return NULL;
    }
    if (is_ota_busy()) {
        printf("[-] Config push skipped: RF OTA in progress on slot %d\n", g_rf_ota.target_slot);
        pthread_mutex_unlock(&push_lock);
        return NULL;
    }
    printf("[*] Starting config push to remote...\n");

    if (g_config_version == 0) {
        FILE *vf = fopen("/data/codex/config_version", "r");
        if (vf) {
            int v = 1;
            if (fscanf(vf, "%d", &v) == 1 && v > 0) g_config_version = (uint8_t)v;
            fclose(vf);
        } else {
            g_config_version = 1;
        }
    }
    uint8_t config_ver = g_config_version;

    /* 1. CONFIG_END (clear) */
    rf_frame_t f;
    uint8_t target_slots[2] = {0};
    int num_targets = 0;
    uint64_t now_ms = get_monotonic_ms();

    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        uint8_t s = g_paired_remotes[i].slot;
        if (g_paired_remotes[i].active && !g_paired_remotes[i].link_lost) {
            if ((g_paired_remotes[i].last_seen_ms > 0 && (now_ms - g_paired_remotes[i].last_seen_ms < 60000)) ||
                (s == g_remote_slot && g_remote_slot != 0)) {
                target_slots[num_targets++] = s;
            }
        }
    }
    if (num_targets == 0) {
        target_slots[num_targets++] = g_remote_slot ? g_remote_slot : 0x02;
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        uint8_t payload[26] = {0};
        payload[0] = 0x00; /* clear */
        payload[1] = config_ver;
        rf_build_frame(&f, target_slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 2);
        rf_send_frame_acked(&f, 300);
    }
    usleep(20000);

    /* 2. Activities */
    struct {
        char id[13];
        char name[24];
        uint8_t icon_type;
    } acts[16];
    int act_count = 0;

    FILE *fa = fopen("/data/codex/config/activities.json", "r");
    if (fa) {
        fseek(fa, 0, SEEK_END);
        long sz = ftell(fa);
        fseek(fa, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fa);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            if (act_count >= 16) break;
                            cJSON *act_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Activity");
                            if (inner && cJSON_IsObject(inner)) act_obj = inner;

                            cJSON *j_name = cJSON_GetObjectItem(act_obj, "Name");
                            if (!j_name) j_name = cJSON_GetObjectItem(act_obj, "name");
                            cJSON *j_id = cJSON_GetObjectItem(act_obj, "Id-");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "id");
                            cJSON *j_icon = cJSON_GetObjectItem(act_obj, "icon");
                            if (!j_icon) j_icon = cJSON_GetObjectItem(act_obj, "Icon");
                            const char *icon_str = (j_icon && cJSON_IsString(j_icon)) ? j_icon->valuestring : "";
                            cJSON *j_type = cJSON_GetObjectItem(act_obj, "type");
                            const char *type_str = (j_type && cJSON_IsString(j_type)) ? j_type->valuestring : "";

                            if (j_name && cJSON_IsString(j_name) && j_id) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(j_id)) snprintf(id_str, sizeof(id_str), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(id_str, j_id->valuestring, sizeof(id_str) - 1);
                                if (strcmp(id_str, "-1") != 0) {
                                    strncpy(acts[act_count].id, id_str, sizeof(acts[0].id) - 1);
                                    acts[act_count].id[sizeof(acts[0].id) - 1] = '\0';
                                    strncpy(acts[act_count].name, j_name->valuestring, sizeof(acts[0].name) - 1);
                                    acts[act_count].name[sizeof(acts[0].name) - 1] = '\0';
                                    acts[act_count].icon_type = map_activity_icon_type(icon_str, j_name->valuestring, type_str);
                                    act_count++;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(fa);
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        for (int i = 0; i < act_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = (uint8_t)act_count;
            payload[2] = acts[i].icon_type;
            if (strlen(acts[i].id) <= 2 || strcmp(acts[i].id, "-1") == 0) {
                /* Short ID format: 3 bytes payload[3..5], up to 19 bytes payload[6..24] for name! */
                strncpy((char *)&payload[3], acts[i].id, 3);
                strncpy((char *)&payload[6], acts[i].name, 19);
            } else {
                /* Legacy fallback: 8-byte ID in payload[3..10], 14-byte name in payload[11..24] */
                strncpy((char *)&payload[3], acts[i].id, 8);
                strncpy((char *)&payload[11], acts[i].name, 14);
            }
            rf_build_frame(&f, target_slot, RF_MSG_ACTIVITY_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_frame_acked(&f, 300);
            usleep(15000);
        }
    }

    /* 3. Devices */
    struct {
        char id[13];
        char name[15];
    } devs[16];
    int dev_count = 0;

    FILE *fd = fopen("/data/codex/config/devices.json", "r");
    if (fd) {
        fseek(fd, 0, SEEK_END);
        long sz = ftell(fd);
        fseek(fd, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fd);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *devs_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "DevicesWithFeatures");
                    if (!devs_arr) devs_arr = cJSON_GetObjectItem(root, "devices");
                    if (devs_arr && cJSON_IsArray(devs_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, devs_arr) {
                            if (dev_count >= 16) break;
                            cJSON *d_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Device");
                            if (inner && cJSON_IsObject(inner)) d_obj = inner;

                            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d_obj, "Id-");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "Id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "id");
                            if (!jid) jid = cJSON_GetObjectItemCaseSensitive(d_obj, "ContentProfileKey");

                            cJSON *jname = cJSON_GetObjectItemCaseSensitive(d_obj, "ParentDeviceModel");
                            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(d_obj, "Name");
                            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(d_obj, "Model");
                            if (!jname) jname = cJSON_GetObjectItemCaseSensitive(d_obj, "FriendlyName");

                            if (jid && jname && cJSON_IsString(jname)) {
                                char id_str[32] = {0};
                                if (cJSON_IsNumber(jid)) snprintf(id_str, sizeof(id_str), "%d", jid->valueint);
                                else if (cJSON_IsString(jid)) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);

                                if (id_str[0]) {
                                    strncpy(devs[dev_count].id, id_str, 12);
                                    devs[dev_count].id[12] = '\0';
                                    strncpy(devs[dev_count].name, jname->valuestring, 14);
                                    devs[dev_count].name[14] = '\0';
                                    dev_count++;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(fd);
    }

    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        for (int i = 0; i < dev_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = (uint8_t)dev_count;
            memcpy(&payload[2], devs[i].id, 10);
            memcpy(&payload[12], devs[i].name, 14);
            rf_build_frame(&f, target_slot, RF_MSG_DEVICE_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_frame_acked(&f, 300);
            usleep(15000);
        }
    }

    /* 4. Buttons: Direct Mode vs Activity Mode per target slot */
    int btn_count = 0;
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        btn_count = 0;
        if (is_slot_direct_mode(target_slot)) {
            /* Direct Device Mode: Push Page 1 and Page 2 screen buttons + direct buttons */
            cJSON *dmap = get_elite_direct_map(target_slot);
            if (dmap) {
                const char *pages[2] = { "page1", "page2" };
                for (int p = 0; p < 2; p++) {
                    cJSON *sbtns = cJSON_GetObjectItem(dmap, pages[p]);
                    if (sbtns && cJSON_IsArray(sbtns)) {
                        int sidx = 0;
                        cJSON *sitem = NULL;
                        cJSON_ArrayForEach(sitem, sbtns) {
                            if (btn_count >= 48) break;
                            if (!cJSON_IsObject(sitem)) continue;
                            cJSON *jlbl = cJSON_GetObjectItem(sitem, "label");
                            const char *label = (jlbl && jlbl->valuestring) ? jlbl->valuestring : "Button";
                            uint8_t payload[26] = {0};
                            payload[0] = (uint8_t)btn_count;
                            payload[1] = (uint8_t)sidx;
                            payload[2] = 0x01; /* screen button */
                            strncpy((char *)&payload[3], pages[p], 9);
                            strncpy((char *)&payload[12], label, 12);
                            payload[24] = 0x01; /* device_cmd */

                            rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                            rf_send_frame_acked(&f, 300);
                            btn_count++;
                            sidx++;
                            usleep(15000);
                        }
                    }
                }
                cJSON_Delete(dmap);
            }
        } else {
            /* Standard Activity Mode */
            cJSON *map = get_elite_map();
            bool has_pwroff = false;
            if (map) {
                pthread_mutex_lock(&g_map_lock);
                cJSON *acts_map = cJSON_GetObjectItemCaseSensitive(map, "activities");
                if (acts_map && cJSON_IsObject(acts_map)) {
                    cJSON *act_entry = NULL;
                    cJSON_ArrayForEach(act_entry, acts_map) {
                        const char *ctx_id = act_entry->string ? act_entry->string : "-1";
                        if (!cJSON_IsObject(act_entry)) continue;

                        cJSON *sbtns = cJSON_GetObjectItemCaseSensitive(act_entry, "screen_buttons");
                        if (sbtns && cJSON_IsArray(sbtns) && cJSON_GetArraySize(sbtns) > 0) {
                            int sidx = 0;
                            cJSON *sitem = NULL;
                            cJSON_ArrayForEach(sitem, sbtns) {
                                if (btn_count >= 48) break;
                                if (!cJSON_IsObject(sitem)) continue;
                                cJSON *jlbl = cJSON_GetObjectItemCaseSensitive(sitem, "label");
                                const char *label = (jlbl && jlbl->valuestring) ? jlbl->valuestring : "Button";

                                uint8_t payload[26] = {0};
                                payload[0] = (uint8_t)btn_count;
                                payload[1] = (uint8_t)sidx;
                                payload[2] = 0x01; /* screen button */
                                strncpy((char *)&payload[3], ctx_id, 9);
                                strncpy((char *)&payload[12], label, 12);
                                payload[24] = 0x01; /* device_cmd */

                                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                                rf_send_frame_acked(&f, 300);
                                btn_count++;
                                sidx++;
                                usleep(15000);
                            }
                        } else {
                            cJSON *b_entry = NULL;
                            int b_idx = 0;
                            cJSON_ArrayForEach(b_entry, act_entry) {
                                if (btn_count >= 48) break;
                                const char *btn_name = b_entry->string;
                                if (!btn_name || !cJSON_IsObject(b_entry)) continue;
                                if (strcmp(btn_name, "screen_buttons") == 0) continue;
                                if (strcasecmp(btn_name, "PwrOff") == 0 || strcasecmp(btn_name, "PowerOffActivity") == 0) {
                                    has_pwroff = true;
                                }

                                cJSON *j_act = cJSON_GetObjectItemCaseSensitive(b_entry, "action");
                                uint8_t atype = 0x01;
                                char target_id[16] = {0};
                                strncpy(target_id, ctx_id, sizeof(target_id) - 1);

                                if (j_act && cJSON_IsString(j_act)) {
                                    if (strcmp(j_act->valuestring, "device_cmd") == 0) {
                                        atype = 0x01;
                                    } else if (strcmp(j_act->valuestring, "activity_start") == 0) {
                                        atype = 0x02;
                                        cJSON *j_actid = cJSON_GetObjectItemCaseSensitive(b_entry, "activityId");
                                        if (j_actid && cJSON_IsString(j_actid)) {
                                            strncpy(target_id, j_actid->valuestring, sizeof(target_id) - 1);
                                        }
                                    } else if (strcmp(j_act->valuestring, "activity_stop") == 0) {
                                        atype = 0x03;
                                        strncpy(target_id, "-1", sizeof(target_id) - 1);
                                    }
                                }

                                uint8_t payload[26] = {0};
                                payload[0] = (uint8_t)btn_count;
                                payload[1] = (uint8_t)b_idx;
                                payload[2] = 0x00; /* physical button */
                                strncpy((char *)&payload[3], target_id, 9);
                                strncpy((char *)&payload[12], btn_name, 12);
                                payload[24] = atype;

                                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                                rf_send_frame_acked(&f, 300);
                                btn_count++;
                                b_idx++;
                                usleep(15000);
                            }
                        }
                    }
                }
                pthread_mutex_unlock(&g_map_lock);
            }
            /* Explicit PowerOff button fallback if not mapped */
            if (!has_pwroff && btn_count < 48) {
                uint8_t payload[26] = {0};
                payload[0] = (uint8_t)btn_count;
                payload[1] = 0;
                payload[2] = 0x00; /* physical button */
                strncpy((char *)&payload[3], "-1", 9);
                strncpy((char *)&payload[12], "PwrOff", 12);
                payload[24] = 0x03; /* activity_stop */

                rf_build_frame(&f, target_slot, RF_MSG_BUTTON_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 25);
                rf_send_frame_acked(&f, 300);
                btn_count++;
                usleep(15000);
            }
        }
    }

    /* 5. Activity Transition Steps & Delays (RF_MSG_STEP_LIST_CHUNK) */
    struct rf_step_send {
        char act_id[12];
        uint8_t step_idx;
        uint8_t total_steps;
        uint16_t delay_ms;
        char name[16];
    };
    struct rf_step_send act_steps[64];
    int total_step_count = 0;

    FILE *fsteps = fopen("/data/codex/config/activities.json", "r");
    if (fsteps) {
        fseek(fsteps, 0, SEEK_END);
        long sz = ftell(fsteps);
        fseek(fsteps, 0, SEEK_SET);
        if (sz > 0 && sz < 1000000) {
            char *buf = malloc(sz + 1);
            if (buf) {
                size_t n = fread(buf, 1, sz, fsteps);
                buf[n] = '\0';
                cJSON *root = cJSON_Parse(buf);
                free(buf);
                if (root) {
                    cJSON *acts_arr = cJSON_IsArray(root) ? root : cJSON_GetObjectItem(root, "Activities");
                    if (!acts_arr) acts_arr = cJSON_GetObjectItem(root, "activities");
                    if (acts_arr && cJSON_IsArray(acts_arr)) {
                        cJSON *elem = NULL;
                        cJSON_ArrayForEach(elem, acts_arr) {
                            if (total_step_count >= 60) break;
                            cJSON *act_obj = elem;
                            cJSON *inner = cJSON_GetObjectItemCaseSensitive(elem, "Activity");
                            if (inner && cJSON_IsObject(inner)) act_obj = inner;

                            cJSON *j_id = cJSON_GetObjectItem(act_obj, "Id-");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "Id");
                            if (!j_id) j_id = cJSON_GetObjectItem(act_obj, "id");
                            char act_id[32] = {0};
                            if (j_id) {
                                if (cJSON_IsNumber(j_id)) snprintf(act_id, sizeof(act_id), "%d", j_id->valueint);
                                else if (cJSON_IsString(j_id)) strncpy(act_id, j_id->valuestring, sizeof(act_id) - 1);
                            }
                            if (!act_id[0]) continue;

                            struct rf_step_send local_steps[8];
                            int local_count = 0;

                            /* startSequence -> Modern Codex format */
                            cJSON *start_seq = cJSON_GetObjectItem(act_obj, "startSequence");
                            if (start_seq && cJSON_IsArray(start_seq)) {
                                cJSON *sitem = NULL;
                                cJSON_ArrayForEach(sitem, start_seq) {
                                    if (local_count >= 8) break;
                                    cJSON *cname = cJSON_GetObjectItem(sitem, "command");
                                    cJSON *ctype = cJSON_GetObjectItem(sitem, "type");
                                    cJSON *cdelay = cJSON_GetObjectItem(sitem, "delay");
                                    int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 0;
                                    if (cname && cJSON_IsString(cname) && cname->valuestring[0]) {
                                        strncpy(local_steps[local_count].act_id, act_id, 11);
                                        local_steps[local_count].act_id[11] = '\0';
                                        strncpy(local_steps[local_count].name, cname->valuestring, 15);
                                        local_steps[local_count].name[15] = '\0';
                                        if (d <= 0) d = 300;
                                        if (d > 20000) d = 20000;
                                        local_steps[local_count].delay_ms = (uint16_t)d;
                                        local_count++;
                                    } else if (ctype && cJSON_IsString(ctype) && strcmp(ctype->valuestring, "Delay") == 0) {
                                        if (local_count > 0 && d > 0) {
                                            local_steps[local_count - 1].delay_ms += (uint16_t)d;
                                        }
                                    }
                                }
                            }

                            /* Roles -> Input selection steps */
                            cJSON *roles = cJSON_GetObjectItem(act_obj, "Roles");
                            if (roles && cJSON_IsArray(roles)) {
                                cJSON *r_item = NULL;
                                cJSON_ArrayForEach(r_item, roles) {
                                    if (local_count >= 3) break;
                                    cJSON *sel_in = cJSON_GetObjectItem(r_item, "SelectedInput");
                                    if (sel_in && cJSON_IsObject(sel_in)) {
                                        cJSON *in_name = cJSON_GetObjectItem(sel_in, "Name");
                                        if (in_name && cJSON_IsString(in_name) && in_name->valuestring[0]) {
                                            strncpy(local_steps[local_count].act_id, act_id, 11);
                                            local_steps[local_count].act_id[11] = '\0';
                                            snprintf(local_steps[local_count].name, sizeof(local_steps[local_count].name), "%s", in_name->valuestring);
                                            local_steps[local_count].delay_ms = 1500;
                                            local_count++;
                                        }
                                    }
                                }
                            }

                            /* EnterActions -> Power / Initial Command steps */
                            cJSON *enter_acts = cJSON_GetObjectItem(act_obj, "EnterActions");
                            if (enter_acts && cJSON_IsArray(enter_acts)) {
                                cJSON *ea = NULL;
                                cJSON_ArrayForEach(ea, enter_acts) {
                                    if (local_count >= 6) break;
                                    cJSON *cname = cJSON_GetObjectItem(ea, "CommandName");
                                    if (!cname) cname = cJSON_GetObjectItem(ea, "Command");
                                    cJSON *cdelay = cJSON_GetObjectItem(ea, "Delay");
                                    if (cname && cJSON_IsString(cname)) {
                                        strncpy(local_steps[local_count].act_id, act_id, 11);
                                        local_steps[local_count].act_id[11] = '\0';
                                        strncpy(local_steps[local_count].name, cname->valuestring, 15);
                                        local_steps[local_count].name[15] = '\0';
                                        int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 1000;
                                        if (d <= 0) d = 300;
                                        if (d > 20000) d = 20000;
                                        local_steps[local_count].delay_ms = (uint16_t)d;
                                        local_count++;
                                    }
                                }
                            }

                            /* Sensible default transition steps if not explicitly in JSON */
                            if (local_count == 0) {
                                strncpy(local_steps[0].act_id, act_id, 11);
                                if (strcmp(act_id, "-1") == 0) {
                                    strncpy(local_steps[0].name, "Power Off", sizeof(local_steps[0].name) - 1);
                                    local_steps[0].delay_ms = 1500;
                                    local_count = 1;
                                } else {
                                    strncpy(local_steps[0].name, "Power On", sizeof(local_steps[0].name) - 1);
                                    local_steps[0].delay_ms = 2500;
                                    strncpy(local_steps[1].act_id, act_id, 11);
                                    strncpy(local_steps[1].name, "Set Inputs", sizeof(local_steps[1].name) - 1);
                                    local_steps[1].delay_ms = 1500;
                                    local_count = 2;
                                }
                            }

                            for (int s = 0; s < local_count && total_step_count < 64; s++) {
                                act_steps[total_step_count] = local_steps[s];
                                act_steps[total_step_count].step_idx = (uint8_t)s;
                                act_steps[total_step_count].total_steps = (uint8_t)local_count;
                                total_step_count++;
                            }

                            /* Append stop/leave sequence step(s) for this activity (tagged with step_idx & 0x80) */
                            if (total_step_count < 63) {
                                cJSON *stop_seq = cJSON_GetObjectItem(act_obj, "stopSequence");
                                if (!stop_seq) stop_seq = cJSON_GetObjectItem(act_obj, "StopSequence");
                                if (!stop_seq) stop_seq = cJSON_GetObjectItem(act_obj, "LeaveActions");
                                int stop_cnt = 0;
                                if (stop_seq && cJSON_IsArray(stop_seq)) {
                                    cJSON *st_item = NULL;
                                    cJSON_ArrayForEach(st_item, stop_seq) {
                                        if (total_step_count >= 63 || stop_cnt >= 8) break;
                                        cJSON *cname = cJSON_GetObjectItem(st_item, "command");
                                        if (!cname) cname = cJSON_GetObjectItem(st_item, "Command");
                                        cJSON *ctype = cJSON_GetObjectItem(st_item, "type");
                                        if (!ctype) ctype = cJSON_GetObjectItem(st_item, "Type");
                                        cJSON *cdelay = cJSON_GetObjectItem(st_item, "delay");
                                        if (!cdelay) cdelay = cJSON_GetObjectItem(st_item, "Delay");
                                        if (!cdelay) cdelay = cJSON_GetObjectItem(st_item, "Duration");
                                        int d = (cdelay && cJSON_IsNumber(cdelay)) ? cdelay->valueint : 0;

                                        if (cname && cJSON_IsString(cname) && cname->valuestring[0]) {
                                            if (d <= 0) d = 250;
                                            if (d > 20000) d = 20000;
                                            strncpy(act_steps[total_step_count].act_id, act_id, 11);
                                            act_steps[total_step_count].act_id[11] = '\0';
                                            strncpy(act_steps[total_step_count].name, cname->valuestring, 15);
                                            act_steps[total_step_count].name[15] = '\0';
                                            act_steps[total_step_count].delay_ms = (uint16_t)d;
                                            act_steps[total_step_count].step_idx = 0x80 | (uint8_t)stop_cnt;
                                            act_steps[total_step_count].total_steps = 1;
                                            total_step_count++;
                                            stop_cnt++;
                                        } else if (ctype && cJSON_IsString(ctype) && strcasecmp(ctype->valuestring, "Delay") == 0) {
                                            if (total_step_count > 0 && stop_cnt > 0 && d > 0) {
                                                act_steps[total_step_count - 1].delay_ms += (uint16_t)d;
                                            }
                                        }
                                    }
                                }
                                if (stop_cnt == 0 && total_step_count < 63 && strcmp(act_id, "-1") != 0) {
                                    strncpy(act_steps[total_step_count].act_id, act_id, 11);
                                    act_steps[total_step_count].act_id[11] = '\0';
                                    strncpy(act_steps[total_step_count].name, "Stop", 15);
                                    act_steps[total_step_count].name[15] = '\0';
                                    act_steps[total_step_count].delay_ms = 1500;
                                    act_steps[total_step_count].step_idx = 0x80;
                                    act_steps[total_step_count].total_steps = 1;
                                    total_step_count++;
                                }
                            }
                        }
                    }
                    cJSON_Delete(root);
                }
            }
        }
        fclose(fsteps);
    }

    /* Append PowerOff sequence step for "-1" */
    if (total_step_count < 64) {
        strncpy(act_steps[total_step_count].act_id, "-1", 11);
        act_steps[total_step_count].act_id[11] = '\0';
        strncpy(act_steps[total_step_count].name, "Power Off", 15);
        act_steps[total_step_count].name[15] = '\0';
        act_steps[total_step_count].delay_ms = 1500;
        act_steps[total_step_count].step_idx = 0;
        act_steps[total_step_count].total_steps = 1;
        total_step_count++;
    }

    /* Send Step Chunks to target slots */
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        if (is_slot_direct_mode(target_slot)) continue;
        for (int i = 0; i < total_step_count; i++) {
            uint8_t payload[26] = {0};
            payload[0] = (uint8_t)i;
            payload[1] = act_steps[i].step_idx;
            payload[2] = act_steps[i].total_steps;
            strncpy((char *)&payload[3], act_steps[i].act_id, 9);
            payload[12] = (uint8_t)(act_steps[i].delay_ms & 0xFF);
            payload[13] = (uint8_t)((act_steps[i].delay_ms >> 8) & 0xFF);
            strncpy((char *)&payload[14], act_steps[i].name, 11);

            rf_build_frame(&f, target_slot, RF_MSG_STEP_LIST_CHUNK, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 26);
            rf_send_frame_acked(&f, 300);
            usleep(15000);
        }
    }

    /* 6. Settings push */
    for (int t = 0; t < num_targets; t++) {
        uint8_t target_slot = target_slots[t];
        int bright = 3, timeout = 120, touch = 1, gyro = 1, nav_key = 139, nav_mode = 1, confirm_act = 0, touch_sens = 2, remote_mode = 0;
        int gyro_rate = 25, btn_backlight = 1, night_start = 22, night_end = 7, usbeth = 0;
        char spath[64];
        snprintf(spath, sizeof(spath), "/data/codex/elite_slot%d_config.json", target_slot);
        FILE *sf = fopen(spath, "r");
        if (sf) {
            char sbuf[4096] = {0};
            size_t sn = fread(sbuf, 1, sizeof(sbuf) - 1, sf);
            sbuf[sn] = '\0';
            fclose(sf);
            cJSON *sroot = cJSON_Parse(sbuf);
            if (sroot) {
                cJSON *jb = cJSON_GetObjectItem(sroot, "brightness");
                cJSON *jt = cJSON_GetObjectItem(sroot, "timeout");
                cJSON *jtouch = cJSON_GetObjectItem(sroot, "touchEnabled");
                if (!jtouch) jtouch = cJSON_GetObjectItem(sroot, "touch");
                cJSON *jgyro = cJSON_GetObjectItem(sroot, "gyroEnabled");
                if (!jgyro) jgyro = cJSON_GetObjectItem(sroot, "gyro");
                cJSON *jnk = cJSON_GetObjectItem(sroot, "navToggleKey");
                if (!jnk) jnk = cJSON_GetObjectItem(sroot, "nav_key");
                cJSON *jnm = cJSON_GetObjectItem(sroot, "navToggleMode");
                if (!jnm) jnm = cJSON_GetObjectItem(sroot, "nav_mode");
                cJSON *jca = cJSON_GetObjectItem(sroot, "confirmActivity");
                if (!jca) jca = cJSON_GetObjectItem(sroot, "confirm_act");
                cJSON *jts = cJSON_GetObjectItem(sroot, "touchSensitivity");
                if (!jts) jts = cJSON_GetObjectItem(sroot, "touch_sensitivity");
                cJSON *jrm = cJSON_GetObjectItem(sroot, "remoteMode");
                if (!jrm) jrm = cJSON_GetObjectItem(sroot, "remote_mode");
                cJSON *jgr = cJSON_GetObjectItem(sroot, "gyroRate");
                if (!jgr) jgr = cJSON_GetObjectItem(sroot, "gyro_rate");
                cJSON *jbb = cJSON_GetObjectItem(sroot, "btnBacklight");
                if (!jbb) jbb = cJSON_GetObjectItem(sroot, "btn_backlight");
                cJSON *jns = cJSON_GetObjectItem(sroot, "nightStart");
                if (!jns) jns = cJSON_GetObjectItem(sroot, "night_start");
                cJSON *jne = cJSON_GetObjectItem(sroot, "nightEnd");
                if (!jne) jne = cJSON_GetObjectItem(sroot, "night_end");
                cJSON *jue = cJSON_GetObjectItem(sroot, "usbEth");
                if (!jue) jue = cJSON_GetObjectItem(sroot, "usbeth");
                cJSON *jnscreen = cJSON_GetObjectItem(sroot, "noScreen");
                if (!jnscreen) jnscreen = cJSON_GetObjectItem(sroot, "no_screen");
                int no_screen = 0;
                if (jnscreen) no_screen = cJSON_IsTrue(jnscreen) || (cJSON_IsNumber(jnscreen) && jnscreen->valueint != 0) ? 1 : 0;
                cJSON *jcap = cJSON_GetObjectItem(sroot, "capButtonsEnabled");
                if (!jcap) jcap = cJSON_GetObjectItem(sroot, "cap_buttons");
                int cap_buttons = 1;
                if (jcap) cap_buttons = (cJSON_IsFalse(jcap) || (cJSON_IsNumber(jcap) && jcap->valueint == 0)) ? 0 : 1;

                if (jb) bright = jb->valueint;
                if (jt) timeout = jt->valueint;
                if (jtouch) touch = cJSON_IsTrue(jtouch) || (cJSON_IsNumber(jtouch) && jtouch->valueint != 0) ? 1 : 0;
                if (jgyro) gyro = cJSON_IsTrue(jgyro) || (cJSON_IsNumber(jgyro) && jgyro->valueint != 0) ? 1 : 0;
                if (jnk) nav_key = jnk->valueint;
                if (jnm) nav_mode = jnm->valueint;
                if (jca) confirm_act = cJSON_IsTrue(jca) || (cJSON_IsNumber(jca) && jca->valueint != 0) ? 1 : 0;
                if (jts && jts->valueint >= 1 && jts->valueint <= 3) touch_sens = jts->valueint;
                if (jrm && jrm->valueint == 1) remote_mode = 1;
                if (jgr && jgr->valueint > 0) gyro_rate = jgr->valueint;
                if (jbb && jbb->valueint >= 0 && jbb->valueint <= 2) btn_backlight = jbb->valueint;
                if (jns && jns->valueint >= 0 && jns->valueint <= 23) night_start = jns->valueint;
                if (jne && jne->valueint >= 0 && jne->valueint <= 23) night_end = jne->valueint;
                if (jue) usbeth = cJSON_IsTrue(jue) || (cJSON_IsNumber(jue) && jue->valueint != 0) ? 1 : 0;
                cJSON_Delete(sroot);
                rf_send_settings_push_slot(target_slot, bright, timeout, touch, gyro, nav_key, nav_mode, confirm_act, touch_sens, remote_mode, gyro_rate, btn_backlight, night_start, night_end, usbeth, no_screen, cap_buttons);
                usleep(15000);
            }
        }

        /* 7. CONFIG_END (complete) */
        uint8_t payload[26] = {0};
        payload[0] = 0x01; /* complete */
        payload[1] = g_config_version;
        rf_build_frame(&f, target_slot, RF_MSG_CONFIG_END, ++g_tx_seq, RF_FLAG_ACK_REQ, payload, 2);
        rf_send_frame_acked(&f, 300);
    }

    printf("[+] Config push complete (acts=%d devs=%d btns=%d steps=%d ver=%d)\n",
           act_count, dev_count, btn_count, total_step_count, g_config_version);
    unlink("/data/codex/rf_sync_pending.flag");
    unlink("/data/codex/rf_sync_pending_details.json");

    pthread_mutex_unlock(&push_lock);
    return NULL;
}

static void ota_set_progress(uint32_t sent_bytes, uint16_t sent_chunks) {
    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.sent_bytes = sent_bytes;
    g_rf_ota.sent_chunks = sent_chunks;
    pthread_mutex_unlock(&g_ota_lock);
}

static void ota_set_status(int status, bool in_prog, const char *err) {
    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.status_code = status;
    g_rf_ota.in_progress = in_prog;
    if (err) {
        snprintf(g_rf_ota.error_msg, sizeof(g_rf_ota.error_msg), "%s", err);
    } else {
        g_rf_ota.error_msg[0] = '\0';
    }
    pthread_mutex_unlock(&g_ota_lock);
    set_hub_led(in_prog ? 2 : 1);
    if (!in_prog) {
        if (status == 2) {
            printf("[+] Hub RF OTA finished: SUCCESS\n");
        } else if (status < 0) {
            printf("[-] Hub RF OTA finished: FAILED (%s)\n", err ? err : "unknown error");
        }
    }
}

/* Background worker: sends binary OTA chunks over RF to Remote */
static void *rf_ota_sender_worker(void *arg) {
    uint8_t target_slot = (uint8_t)(uintptr_t)arg;
    if (target_slot < 1 || target_slot > 2) target_slot = g_remote_slot ? g_remote_slot : 1;

    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.in_progress = true;
    g_rf_ota.target_slot = target_slot;
    g_rf_ota.status_code = 1; /* running */
    g_rf_ota.error_msg[0] = '\0';
    g_rf_ota.sent_bytes = 0;
    g_rf_ota.sent_chunks = 0;
    g_rf_ota.total_bytes = 0;
    g_rf_ota.total_chunks = 0;
    pthread_mutex_unlock(&g_ota_lock);

    const char *ota_path = "/cache/codex_elite.ota.gz";
    if (access(ota_path, R_OK) != 0) ota_path = "/data/codex_elite.ota.gz";
    if (access(ota_path, R_OK) != 0) ota_path = "/tmp/codex_elite.ota.gz";
    FILE *f = fopen(ota_path, "rb");
    if (!f) {
        char err[128];
        snprintf(err, sizeof(err), "OTA image not found (%s)", ota_path);
        ota_set_status(-1, false, err);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > 10000000) {
        fclose(f);
        char err[128];
        snprintf(err, sizeof(err), "Invalid OTA file size: %ld", file_size);
        ota_set_status(-1, false, err);
        return NULL;
    }

    uint32_t tot_bytes = (uint32_t)file_size;
    uint16_t tot_chunks = (uint16_t)((tot_bytes + 21) / 22);

    pthread_mutex_lock(&g_ota_lock);
    g_rf_ota.total_bytes = tot_bytes;
    g_rf_ota.total_chunks = tot_chunks;
    pthread_mutex_unlock(&g_ota_lock);

    printf("[*] Starting RF OTA to slot %d: %u bytes, %u chunks from %s\n",
           target_slot, tot_bytes, tot_chunks, ota_path);

    /* 1. RF_MSG_OTA_START */
    rf_frame_t frame;
    uint8_t start_payload[25] = {0};
    start_payload[0] = (uint8_t)(tot_bytes & 0xFF);
    start_payload[1] = (uint8_t)((tot_bytes >> 8) & 0xFF);
    start_payload[2] = (uint8_t)((tot_bytes >> 16) & 0xFF);
    start_payload[3] = (uint8_t)((tot_bytes >> 24) & 0xFF);
    start_payload[4] = (uint8_t)(tot_chunks & 0xFF);
    start_payload[5] = (uint8_t)((tot_chunks >> 8) & 0xFF);

    int ack_res = -1;
    for (int attempt = 1; attempt <= 10 && g_running; attempt++) {
        rf_build_frame(&frame, target_slot, RF_MSG_OTA_START, ++g_tx_seq, RF_FLAG_ACK_REQ, start_payload, 6);
        ack_res = rf_send_frame_acked(&frame, 1000);
        if (ack_res == 0) break;
        if (attempt < 10) {
            printf("[*] Waiting for remote in slot %d to respond to OTA_START (attempt %d/10)...\n",
                   target_slot, attempt);
            usleep(800000); /* 800ms between retries */
        }
    }

    if (ack_res != 0) {
        fclose(f);
        ota_set_status(-1, false, "Remote failed to ACK OTA_START (timeout - wake remote)");
        return NULL;
    }
    if (g_ack_status != 0x00) {
        fclose(f);
        char err[128];
        if (g_ack_status == 0x02) {
            snprintf(err, sizeof(err), "Remote rejected OTA: Low battery (<40%%)");
        } else {
            snprintf(err, sizeof(err), "Remote rejected OTA: Error code 0x%02X", g_ack_status);
        }
        ota_set_status(-1, false, err);
        return NULL;
    }

    /* 2. Stream RF_MSG_OTA_CHUNK frames */
    uint16_t c_idx = 0;
    uint32_t sent_bytes = 0;
    while (c_idx < tot_chunks && g_running) {
        uint8_t chunk_data[22];
        size_t rd = fread(chunk_data, 1, sizeof(chunk_data), f);
        if (rd == 0) break;

        uint8_t chunk_payload[25] = {0};
        chunk_payload[0] = (uint8_t)(c_idx & 0xFF);
        chunk_payload[1] = (uint8_t)((c_idx >> 8) & 0xFF);
        chunk_payload[2] = (uint8_t)rd;
        memcpy(&chunk_payload[3], chunk_data, rd);

        bool need_ack = ((c_idx + 1 == tot_chunks) || (c_idx % 4 == 0));
        uint8_t flags = need_ack ? RF_FLAG_ACK_REQ : 0;
        rf_build_frame(&frame, target_slot, RF_MSG_OTA_CHUNK, ++g_tx_seq, flags, chunk_payload, (uint8_t)(3 + rd));

        if (need_ack) {
            int retries = 0;
            int ack_ok = -1;
            while (retries < 5) {
                if (rf_send_frame_acked(&frame, 1200) == 0 && g_ack_status == 0x00) {
                    ack_ok = 0;
                    break;
                }
                retries++;
                usleep(35000);
            }
            if (ack_ok != 0) {
                fclose(f);
                char err[128];
                snprintf(err, sizeof(err), "Chunk %u ACK timeout", c_idx);
                ota_set_status(-1, false, err);
                return NULL;
            }
        } else {
            rf_send_frame(&frame);
        }

        sent_bytes += rd;
        ota_set_progress(sent_bytes, c_idx + 1);
        if ((c_idx + 1) % 250 == 0 || c_idx + 1 == tot_chunks) {
            uint32_t pct = tot_chunks > 0 ? (uint32_t)(((c_idx + 1) * 100) / tot_chunks) : 0;
            printf("[*] Hub RF OTA to Slot %d: %u%% (%u/%u chunks, %u/%u bytes)\n",
                   target_slot, pct, c_idx + 1, tot_chunks, sent_bytes, tot_bytes);
        }
        c_idx++;
        usleep(15000); /* 15ms pace to avoid CC2544 FIFO overflow */
    }

    fclose(f);

    /* 3. RF_MSG_OTA_END */
    uint8_t end_payload[25] = {0};
    rf_build_frame(&frame, target_slot, RF_MSG_OTA_END, ++g_tx_seq, RF_FLAG_ACK_REQ, end_payload, 1);
    if (rf_send_frame_acked(&frame, 6000) != 0) {
        ota_set_status(-1, false, "OTA_END ACK timeout");
        return NULL;
    }

    if (g_ack_status != 0x00) {
        char err[128];
        snprintf(err, sizeof(err), "Remote verification failed (status 0x%02X)", g_ack_status);
        ota_set_status(-1, false, err);
        return NULL;
    }

    printf("[+] RF OTA completed successfully to slot %d!\n", target_slot);
    ota_set_status(2, false, NULL);
    return NULL;
}

/* Handle incoming Codex RF protocol frame */
static void handle_codex_frame(const rf_frame_t *f) {
    if (f->dev_slot == 1 || f->dev_slot == 2) {
        mark_remote_slot_active(f->dev_slot);
    }
    switch (f->msg_type) {
        case RF_MSG_ACK: {
            pthread_mutex_lock(&g_ack_mutex);
            uint8_t acked = f->payload[0];
            if ((acked & 0x0F) == (g_pending_ack_seq & 0x0F) || acked == g_pending_ack_seq) {
                g_ack_received = 1;
                g_ack_status = f->payload[1];
                pthread_cond_signal(&g_ack_cond);
            }
            pthread_mutex_unlock(&g_ack_mutex);
            break;
        }

        case RF_MSG_BUTTON_PRESS: {
            uint8_t slot = (f->dev_slot & 0x0F);
            if (slot < 1 || slot > 2) slot = 1;
            uint16_t key_code = ((uint16_t)f->payload[0] << 8) | f->payload[1];
            uint8_t state = f->payload[2];
            char btn_name[13] = {0};
            memcpy(btn_name, &f->payload[3], 12);

            const char *action = (state == 0x01) ? "press" : ((state == 0x02) ? "hold" : "release");
            const char *name = map_button_code(key_code >> 8, key_code & 0xFF);
            if (!name) name = btn_name[0] ? btn_name : "Unknown";

            printf("[*] Codex BUTTON_PRESS: slot=%d code=0x%04X state=%s name=%s\n", slot, key_code, action, name);
            enqueue_elite_button(slot, name, action);

            static uint32_t s_last_sync_ms = 0;
            uint32_t now_ms = (uint32_t)get_monotonic_ms();
            if (!is_ota_busy() && (now_ms - s_last_sync_ms > 5000)) {
                s_last_sync_ms = now_ms;
                char cur[32] = "-1";
                get_current_activity(cur, sizeof(cur));
                rf_send_codex_activity_sync(cur);
            }
            break;
        }

        case RF_MSG_ACTIVITY_REQUEST: {
            char act_id[13] = {0};
            memcpy(act_id, f->payload, 12);
            printf("[*] Codex ACTIVITY_REQUEST: %s\n", act_id);
            enqueue_elite_activity(act_id);
            break;
        }

        case RF_MSG_DEVICE_CMD_REQUEST: {
            uint8_t slot = (f->dev_slot & 0x0F);
            if (slot < 1 || slot > 2) slot = 1;
            char act_or_dev[13] = {0};
            memcpy(act_or_dev, f->payload, 10);
            uint8_t btn_idx = f->payload[10];

            bool handled = false;
            if (is_slot_direct_mode(slot)) {
                cJSON *dmap = get_elite_direct_map(slot);
                if (dmap) {
                    const char *page_key = (strcmp(act_or_dev, "page2") == 0) ? "page2" : "page1";
                    cJSON *sbtns = cJSON_GetObjectItem(dmap, page_key);
                    if (sbtns && cJSON_IsArray(sbtns)) {
                        cJSON *item = cJSON_GetArrayItem(sbtns, btn_idx);
                        if (item && cJSON_IsObject(item)) {
                            cJSON *jdev = cJSON_GetObjectItem(item, "deviceId");
                            cJSON *jcmd = cJSON_GetObjectItem(item, "command");
                            if (jdev && jcmd && jdev->valuestring && jcmd->valuestring) {
                                printf("[*] Direct Screen button dispatch: slot=%d page=%s idx=%d -> dev=%s cmd=%s\n",
                                       slot, page_key, btn_idx, jdev->valuestring, jcmd->valuestring);
                                enqueue_elite_device_cmd(jdev->valuestring, jcmd->valuestring);
                                handled = true;
                            }
                        }
                    }
                    cJSON_Delete(dmap);
                }
            } else {
                /* Check if this is a screen button index lookup in elite_remote_map.json */
                cJSON *map = get_elite_map();
                if (map) {
                    pthread_mutex_lock(&g_map_lock);
                    cJSON *acts = cJSON_GetObjectItemCaseSensitive(map, "activities");
                    if (acts && cJSON_IsObject(acts)) {
                        cJSON *act_obj = cJSON_GetObjectItemCaseSensitive(acts, act_or_dev);
                        if (!act_obj && strcmp(act_or_dev, "-1") == 0) {
                            act_obj = cJSON_GetObjectItemCaseSensitive(acts, "-1");
                        }
                        if (act_obj && cJSON_IsObject(act_obj)) {
                            cJSON *sbtns = cJSON_GetObjectItemCaseSensitive(act_obj, "screen_buttons");
                            if (sbtns && cJSON_IsArray(sbtns)) {
                                cJSON *item = cJSON_GetArrayItem(sbtns, btn_idx);
                                if (item && cJSON_IsObject(item)) {
                                    cJSON *jdev = cJSON_GetObjectItemCaseSensitive(item, "deviceId");
                                    cJSON *jcmd = cJSON_GetObjectItemCaseSensitive(item, "command");
                                    if (jdev && jdev->valuestring && jcmd && jcmd->valuestring) {
                                        printf("[*] Screen button dispatch: act=%s idx=%d -> dev=%s cmd=%s\n",
                                               act_or_dev, btn_idx, jdev->valuestring, jcmd->valuestring);
                                        enqueue_elite_device_cmd(jdev->valuestring, jcmd->valuestring);
                                        handled = true;
                                    }
                                }
                            }
                        }
                    }
                    pthread_mutex_unlock(&g_map_lock);
                }
            }

            if (!handled) {
                /* Fallback to direct dev_id / command */
                char dev_id[13] = {0};
                char cmd[17] = {0};
                memcpy(dev_id, f->payload, 12);
                memcpy(cmd, &f->payload[12], 16);
                printf("[*] Codex DEVICE_CMD direct: dev=%s cmd=%s\n", dev_id, cmd);
                enqueue_elite_device_cmd(dev_id, cmd);
            }
            break;
        }

        case RF_MSG_SYNC_REQUEST: {
            if (is_ota_busy()) {
                printf("[*] RF OTA active - suppressing SYNC_REQUEST response\n");
                break;
            }
            uint8_t req_type = f->payload[0];
            uint8_t slot = f->dev_slot ? f->dev_slot : (g_remote_slot ? g_remote_slot : 0x01);
            printf("[*] Codex SYNC_REQUEST: type=%d seq=%d slot=%d\n", req_type, f->seq, slot);

            char cur[32] = "-1";
            get_current_activity(cur, sizeof(cur));
            rf_send_codex_activity_sync_slot(cur, f->seq, slot, 0x01);

            if (req_type == 0x02) {
                usleep(50000);
                pthread_t push_th;
                pthread_create(&push_th, NULL, config_push_worker, NULL);
                pthread_detach(push_th);
            } else {
                usleep(10000);
                rf_send_codex_time_sync_slot(slot);
            }
            break;
        }

        case RF_MSG_BATTERY_STATUS: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            int idx = slot - 1;
            g_remote_batteries[idx].percent = f->payload[0];
            g_remote_batteries[idx].charging = f->payload[1];
            g_remote_batteries[idx].voltage_mv = (uint16_t)f->payload[2] | ((uint16_t)f->payload[3] << 8);
            g_remote_batteries[idx].last_update_ms = get_monotonic_ms();
            printf("[*] Codex BATTERY_STATUS: Slot %d: %d%% (chg=%d, %umV)\n",
                   slot, g_remote_batteries[idx].percent, g_remote_batteries[idx].charging, g_remote_batteries[idx].voltage_mv);
            publish_remote_battery_mqtt(slot, g_remote_batteries[idx].percent, g_remote_batteries[idx].charging, g_remote_batteries[idx].voltage_mv);
            if (!is_ota_busy()) {
                rf_frame_t ack;
                rf_build_ack(&ack, slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            break;
        }

        case RF_MSG_REMOTE_CONFIG_BACKUP: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            printf("[*] Codex REMOTE_CONFIG_BACKUP: Slot %d\n", slot);
            save_remote_config_backup(slot, f->payload);
            if (!is_ota_busy()) {
                rf_frame_t ack;
                rf_build_ack(&ack, slot, f->seq, 0x00);
                rf_send_frame(&ack);
            }
            break;
        }

        case RF_MSG_VERSION_REPORT: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            uint8_t maj = f->payload[0];
            uint8_t min = f->payload[1];
            uint8_t pat = f->payload[2];
            uint8_t cfg_ver = f->payload[3];
            int idx = slot - 1;
            snprintf(g_remote_versions[idx].sw_version, sizeof(g_remote_versions[idx].sw_version), "%u.%u.%u", maj, min, pat);
            g_remote_versions[idx].config_version = cfg_ver;
            g_remote_versions[idx].last_report_ms = get_monotonic_ms();

            char vpath[64];
            snprintf(vpath, sizeof(vpath), "/data/codex/elite_slot%d_version.json", slot);
            FILE *vf = fopen(vpath, "w");
            if (vf) {
                fprintf(vf, "{\"slot\":%d,\"version\":\"%s\",\"config_version\":%d,\"timestamp\":%ld}\n",
                        slot, g_remote_versions[idx].sw_version, cfg_ver, (long)time(NULL));
                fclose(vf);
            }
            printf("[*] Codex VERSION_REPORT: Slot %d -> SW v%s, Config v%d\n", slot, g_remote_versions[idx].sw_version, cfg_ver);
            rf_frame_t ack;
            rf_build_ack(&ack, slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_UNPAIR_NOTIFY: {
            uint8_t slot = f->dev_slot & 0x0F;
            if (slot < 1 || slot > 2) slot = 1;
            RF_LOG(LOG_LEVEL_INFO, "[*] Codex UNPAIR_NOTIFY received from Remote Slot %d\n", slot);
            uint8_t s = (slot == 2) ? 0x02 : 0x01;
            uint8_t unpair[7] = {0x10, 0xFF, 0x80, 0xB2, 0x03, s, 0x00};
            rf_write_cmd(unpair, sizeof(unpair));
            g_paired_remotes[slot - 1].active = false;
            memset(g_paired_remotes[slot - 1].rf_addr, 0, 4);
            if (!g_paired_remotes[0].active && !g_paired_remotes[1].active) {
                g_has_paired_device = false;
                memset(g_paired_addr, 0, sizeof(g_paired_addr));
            }
            rf_frame_t ack;
            rf_build_ack(&ack, slot, f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        case RF_MSG_PING: {
            rf_frame_t ack;
            rf_build_ack(&ack, f->dev_slot ? f->dev_slot : (g_remote_slot ? g_remote_slot : 0x01), f->seq, 0x00);
            rf_send_frame(&ack);
            break;
        }

        default:
            RF_LOG(LOG_LEVEL_DEBUG, "[*] Codex RF unknown type 0x%02X\n", f->msg_type);
            break;
    }
}

/* Query paired devices state across CC2544 pipes matching stock hal_hub */
static void query_paired_devices(void) {
    uint8_t q_pipes[] = { 0x20, 0x30, 0x40, 0x21, 0x31, 0x41, 0x03 };
    for (size_t i = 0; i < sizeof(q_pipes); i++) {
        uint8_t q[7] = {0x10, 0xFF, 0x83, 0xB5, q_pipes[i], 0x00, 0x00};
        rf_write_cmd(q, sizeof(q));
        usleep(30000);
    }
}

/* Parse incoming packet from CC2544 */
static void handle_rf_packet(const uint8_t *pkt, size_t len) {
    if (len < 7) return;

    if (pkt[0] == 0x20 && (pkt[1] & 0x0F) > 0 && (pkt[1] & 0x0F) <= 2) {
        mark_remote_slot_active(pkt[1] & 0x0F);
    } else if (pkt[0] == 0x12 && pkt[1] >= 1 && pkt[1] <= 2) {
        mark_remote_slot_active(pkt[1]);
    }

    if (g_debug_logging) {
        printf("[*] RX (len=%zd): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", pkt[i]);
        printf("\n");
    }

    /* Route Codex RF protocol frames */
    if (rf_is_valid_frame(pkt, len)) {
        rf_frame_t f;
        memset(&f, 0, sizeof(f));
        if (pkt[0] == 0x20 && len >= 32) {
            if ((pkt[4] & 0xE0) == 0x40 || (pkt[4] & 0xE0) == 0x60) {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] & 0x0F;
                f.sub_id = 0x12;
                f.seq = pkt[4] & 0x0F;
                f.hdr_flag = pkt[4];
                f.msg_type = pkt[5];
                f.flags = pkt[6];
                memcpy(f.payload, &pkt[7], sizeof(f.payload));
            } else {
                memcpy(&f, pkt, sizeof(f));
            }
        } else if (pkt[0] == 0x12 && len >= 30) {
            if (pkt[1] >= 1 && pkt[1] <= 15) {
                g_remote_slot = pkt[1];
            }
            if ((pkt[2] & 0xE0) == 0x40 || (pkt[2] & 0xE0) == 0x60) {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] ? pkt[1] : (g_remote_slot ? g_remote_slot : 0x01);
                f.sub_id = 0x12;
                f.seq = pkt[2] & 0x0F;
                f.hdr_flag = pkt[2];
                f.msg_type = pkt[3];
                f.flags = pkt[4];
                memcpy(f.payload, &pkt[5], sizeof(f.payload));
            } else {
                f.report_id = 0x20;
                f.dev_slot = pkt[1] ? pkt[1] : (g_remote_slot ? g_remote_slot : 0x01);
                f.sub_id = 0x12;
                f.seq = pkt[2] & 0x0F;
                f.hdr_flag = 0x40;
                f.msg_type = pkt[2];
                f.flags = pkt[3];
                memcpy(f.payload, &pkt[4], sizeof(f.payload));
            }
            g_nak_count = 0;
            g_backoff_ms = 0;
        }
        handle_codex_frame(&f);
        return;
    }

    /* 0x42: CC2544 hardware TX completion report: 20 <slot> 42 <status> ... */
    if (len >= 4 && pkt[0] == 0x20 && pkt[2] == 0x42) {
        uint8_t status = pkt[3];
        if (status == 0x00) {
            pthread_mutex_lock(&g_ack_mutex);
            g_ack_received = 1;
            pthread_cond_signal(&g_ack_cond);
            pthread_mutex_unlock(&g_ack_mutex);
            if (g_debug_logging)
                printf("[+] CC2544 Hardware ACK (0x42) for seq=%d\n", g_pending_ack_seq);
        } else if (status != 0x01) {
            if (g_debug_logging)
                printf("[-] CC2544 Hardware TX status=0x%02X\n", status);
        }
        return;
    }

    /* 0x41: Link status notification: 10 <slot> 41 04 <status> ... */
    if (len == 7 && pkt[0] == 0x10 && pkt[2] == 0x41) {
        if (pkt[1] >= 1 && pkt[1] <= 15) {
            g_remote_slot = pkt[1];
        }
        bool link_lost = (pkt[4] & 0x40) != 0;
        if (pkt[1] == 1 || pkt[1] == 2) {
            g_paired_remotes[pkt[1] - 1].link_lost = link_lost;
        }
        if (g_debug_logging)
            printf("[*] Hub RF Link Status: %s (slot=%d byte4=0x%02X)\n",
                   link_lost ? "LOST" : "ESTABLISHED", pkt[1], pkt[4]);
        return;
    }

    /* Reset NAK backoff on any valid non-error response from CC2544 */
    if (!(pkt[0] == 0x10 && len >= 3 && pkt[2] == 0x8F)) {
        if (g_nak_count > 0 && g_debug_logging)
            printf("[+] NAK backoff cleared (was %d NAKs, %dms)\n", g_nak_count, g_backoff_ms);
        g_nak_count = 0;
        g_backoff_ms = 0;
    }

    uint8_t report_id = pkt[0];

    /* Short Report: 7 bytes */
    if (report_id == 0x10) {
        uint8_t sub_id = pkt[2];

        /* Firmware Version Reply */
        if (sub_id == 0x81 && pkt[3] == 0xF1) {
            g_fw_version = pkt[5];
            printf("[+] CC2544 FW Version: v%d.00 (0x%02X)\n", g_fw_version, g_fw_version);
            return;
        }

        /* 0x4A: Pairing Notification / Status */
        if (sub_id == 0x4A) {
            uint8_t pair_code = pkt[3];
            uint8_t pair_status = pkt[4];
            printf("[*] CC2544 Pairing event: code=0x%02X status=0x%02X (pkt: %02X %02X %02X %02X %02X %02X %02X)\n",
                   pair_code, pair_status, pkt[0], pkt[1], pkt[2], pkt[3], pkt[4], pkt[5], pkt[6]);
            if (pair_code == 0x00 && pair_status == 0x00) {
                printf("[+] CC2544 Pairing SUCCESS! Remote paired to Hub!\n");
                query_paired_devices();
                stop_pairing_mode();
            } else if (pair_code >= 0x01 && pair_status == 0x00) {
                printf("[+] CC2544 Pairing lock is OPEN (listening for remotes on slot=%d)...\n", pair_code);
                /* Stay in pairing mode - do NOT close lock! */
            } else {
                printf("[*] CC2544 Pairing in-progress / event: code=0x%02X status=0x%02X\n", pair_code, pair_status);
            }
            return;
        }

        /* 0x8F: Error / NAK — trigger exponential backoff */
        if (sub_id == 0x8F) {
            g_nak_count++;
            if (g_nak_count > 3) {
                if (g_nak_count - 3 >= 20) {
                    g_backoff_ms = NAK_BACKOFF_MAX_MS;
                } else {
                    g_backoff_ms = 100 * (1 << (g_nak_count - 3));
                    if (g_backoff_ms > NAK_BACKOFF_MAX_MS) g_backoff_ms = NAK_BACKOFF_MAX_MS;
                }
            }
            if (g_debug_logging || g_nak_count <= 5)
                printf("[-] CC2544 NAK: SubID=0x%02X err=0x%02X (count=%d, backoff=%dms)\n",
                       pkt[3], pkt[5], g_nak_count, g_backoff_ms);
            return;
        }
    }

    /* Long Report: 20 bytes */
    if (report_id == 0x11) {
        uint8_t sub_id = pkt[2];

        /* Paired Device Info replies (0x83 0xB5) matching stock hal_hub libhal_get_paired_device_info */
        if (sub_id == 0x83 && pkt[3] == 0xB5) {
            uint8_t pipe = pkt[4];
            if (pipe == 0x03) {
                memcpy(g_hub_rf_addr, &pkt[5], 4);
                if (g_debug_logging)
                    printf("[+] Hub CC2544 Own RF Address: %02X %02X %02X %02X\n",
                           pkt[5], pkt[6], pkt[7], pkt[8]);
                return;
            }
            if (pipe >= 0x20 && pipe <= 0x25) {
                int sidx = pipe - 0x20;
                uint16_t equad_id = ((uint16_t)pkt[7] << 8) | pkt[8];
                if (sidx < MAX_PAIRED_REMOTES && equad_id != 0) {
                    g_paired_remotes[sidx].active = true;
                    g_has_paired_device = true;
                    printf("[+] Slot %d identified: EquadID=0x%04X\n", sidx + 1, equad_id);
                }
                return;
            }
            if (pipe >= 0x30 && pipe <= 0x35) {
                int sidx = pipe - 0x30;
                if (sidx < MAX_PAIRED_REMOTES) {
                    memcpy(g_paired_remotes[sidx].rf_addr, &pkt[5], 4);
                    bool has_addr = (pkt[5] != 0 || pkt[6] != 0 || pkt[7] != 0 || pkt[8] != 0);
                    g_paired_remotes[sidx].active = has_addr;
                    if (has_addr) g_has_paired_device = true;
                    if (sidx == 0 && has_addr) memcpy(g_paired_addr, &pkt[5], 4);
                    printf("[+] Slot %d RF Address: %02X %02X %02X %02X (active=%d)\n",
                           sidx + 1, pkt[5], pkt[6], pkt[7], pkt[8], g_paired_remotes[sidx].active);
                }
                return;
            }
            if (pipe >= 0x40 && pipe <= 0x45) {
                int sidx = pipe - 0x40;
                uint8_t nlen = pkt[5];
                if (sidx < MAX_PAIRED_REMOTES && nlen > 0) {
                    if (nlen > 15) nlen = 15;
                    memcpy(g_paired_remotes[sidx].name, &pkt[6], nlen);
                    g_paired_remotes[sidx].name[nlen] = '\0';
                    printf("[+] Slot %d Device Name: '%s'\n", sidx + 1, g_paired_remotes[sidx].name);
                }
                return;
            }
        }
    }

    /* Generic packet log */
    if (g_debug_logging) {
        printf("[*] RX (%zd bytes): ", len);
        for (size_t i = 0; i < len; i++) printf("%02X ", pkt[i]);
        printf("\n");
    }
}

/* Background worker: syncs Hub current_activity to Remote on change */
static void *activity_sync_worker(void *arg) {
    (void)arg;
    char last_act[32] = "";

    /* Wait for pairing response from CC2544 */
    for (int i = 0; i < 20 && g_running && !g_has_paired_device; i++) {
        usleep(100000);
    }

    /* Seed last_act on startup to prevent unsolicited TX into empty air */
    get_current_activity(last_act, sizeof(last_act));

    while (g_running) {
        char cur_act[32] = "-1";
        get_current_activity(cur_act, sizeof(cur_act));
        if (strcmp(cur_act, last_act) != 0) {
            if (!is_ota_busy()) {
                strncpy(last_act, cur_act, sizeof(last_act) - 1);
                last_act[sizeof(last_act) - 1] = '\0';
                rf_send_codex_activity_sync_seq(cur_act, ++g_tx_seq);
            }
        }
        sleep(1);
    }
    return NULL;
}

/* Background receiver thread (blocking read on /dev/rfspi) */
static void *rx_worker(void *arg) {
    (void)arg;
    uint8_t buf[64];

    while (g_running) {
        memset(buf, 0, sizeof(buf));
        ssize_t n = read(g_rf_fd, buf, sizeof(buf));
        if (n > 0) {
            handle_rf_packet(buf, (size_t)n);
        } else if (n < 0) {
            if (errno == EINTR) continue;
            perror("read /dev/rfspi");
            usleep(100000);
        } else {
            usleep(50000);
        }
    }
    return NULL;
}

static void set_hub_led(int state) {
    int fd = open("/sys/class/input/input0/led", O_WRONLY);
    if (fd >= 0) {
        /* On Harmony Hub Atheros input0: 0 is Off, 1 is Solid Red, 2 is Fast Blink, 3 is Slow Blink */
        const char *val = (state == 0) ? "0\n" : "1\n";
        ssize_t w = write(fd, val, strlen(val));
        (void)w;
        close(fd);
    }
}

static void *led_blink_worker(void *arg) {
    (void)arg;
    int state = 0;
    while (g_running && g_pairing_mode) {
        set_hub_led(state ? 1 : 0); /* Blink Red during pairing */
        state = !state;
        usleep(250000);
    }
    set_hub_led(1); /* Solid Red when ready */
    return NULL;
}

static void *pairing_timeout_worker(void *arg) {
    (void)arg;
    for (int i = 0; i < 30 && g_running && g_pairing_mode; i++) {
        sleep(1);
    }
    if (g_pairing_mode) {
        printf("[*] Pairing mode timed out after 30s\n");
        stop_pairing_mode();
    }
    return NULL;
}

static void start_pairing_mode(int slot) {
    if (g_pairing_mode) return;
    (void)slot;
    uint8_t duration_sec = 0x1E; /* 30 seconds lock duration (matches stock hal_hub libhal_rf_host_open_lock) */
    uint8_t open_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x01, 0x00, duration_sec};
    printf("[*] Opening pairing lock (duration=%ds, 10 FF 80 B2 01 00 %02X)...\n", duration_sec, duration_sec);
    rf_write_cmd(open_lock, sizeof(open_lock));
    g_pairing_mode = true;

    pthread_t led_th;
    if (pthread_create(&led_th, NULL, led_blink_worker, NULL) == 0) {
        pthread_detach(led_th);
    }

    pthread_t timer_th;
    if (pthread_create(&timer_th, NULL, pairing_timeout_worker, NULL) == 0) {
        pthread_detach(timer_th);
    }
}

static void stop_pairing_mode(void) {
    if (g_pairing_mode) {
        g_pairing_mode = false;
        uint8_t close_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
        printf("[*] Closing pairing lock (10 FF 80 B2 02 00 00)...\n");
        rf_write_cmd(close_lock, sizeof(close_lock));
        set_hub_led(1);
    }
}

static void *hub_button_worker(void *arg) {
    (void)arg;
    int btn_fd = open("/dev/input/event0", O_RDONLY);
    if (btn_fd < 0) {
        printf("[-] Warning: cannot open /dev/input/event0 for Hub Pair button: %s\n", strerror(errno));
        return NULL;
    }
    printf("[+] Hub Pair button listener started on /dev/input/event0\n");

    struct input_event ev;
    while (g_running) {
        ssize_t n = read(btn_fd, &ev, sizeof(ev));
        if (n == (ssize_t)sizeof(ev)) {
            if (ev.type == 1 && ev.value == 1) { /* EV_KEY press */
                printf("[*] Hub Physical Pair Button Pressed! (code=%d)\n", ev.code);
                if (g_pairing_mode) {
                    printf("[*] Pairing currently active -> Stopping pairing mode\n");
                    stop_pairing_mode();
                } else {
                    printf("[*] Starting pairing mode from Hub Pair Button\n");
                    int target_slot = (g_paired_remotes[0].active && !g_paired_remotes[1].active) ? 2 : 0;
                    start_pairing_mode(target_slot);
                }
            }
        } else if (n < 0) {
            if (errno == EINTR) continue;
            usleep(100000);
        }
    }
    close(btn_fd);
    return NULL;
}

static void unpair_device(int slot) {
    /* Send RF notification to remote before unpairing so remote clears bonding */
    rf_frame_t unpair_msg;
    memset(&unpair_msg, 0, sizeof(unpair_msg));
    unpair_msg.report_id = 0x20;
    unpair_msg.sub_id = 0x12;
    unpair_msg.msg_type = RF_MSG_UNPAIR_NOTIFY;
    unpair_msg.hdr_flag = 0x50 | (g_tx_seq & 0x0F);
    unpair_msg.seq = g_tx_seq++;
    if (slot == 1 || slot == 0) {
        unpair_msg.dev_slot = 0x01;
        rf_send_frame(&unpair_msg);
    }
    if (slot == 2 || slot == 0) {
        unpair_msg.dev_slot = 0x02;
        rf_send_frame(&unpair_msg);
    }
    usleep(30000); /* Allow RF frame transmission before hardware lock reset */

    uint8_t s = (slot == 2) ? 0x02 : ((slot == 1) ? 0x01 : 0x00);
    uint8_t unpair[] = { 0x10, 0xFF, 0x80, 0xB2, 0x03, s, 0x00 };
    RF_LOG(LOG_LEVEL_INFO, "[*] Unpairing remote (slot=%d, 10 FF 80 B2 03 %02X 00)...\n", slot, s);
    rf_write_cmd(unpair, sizeof(unpair));
    if (slot == 1 || slot == 0) {
        g_paired_remotes[0].active = false;
        memset(g_paired_remotes[0].rf_addr, 0, 4);
    }
    if (slot == 2 || slot == 0) {
        g_paired_remotes[1].active = false;
        memset(g_paired_remotes[1].rf_addr, 0, 4);
    }
    if (!g_paired_remotes[0].active && !g_paired_remotes[1].active) {
        g_has_paired_device = false;
        memset(g_paired_addr, 0, sizeof(g_paired_addr));
    }
}

/* Minimal HTTP server on 8092 for WebUI / CLI control */
static void *api_server_worker(void *arg) {
    (void)arg;
    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) return NULL;

    fcntl(srv, F_SETFD, FD_CLOEXEC);

    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(RF_API_PORT);
    sin.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv, (struct sockaddr *)&sin, sizeof(sin)) < 0) {
        close(srv);
        return NULL;
    }
    listen(srv, 4);

    while (g_running) {
        int cli = accept(srv, NULL, NULL);
        if (cli < 0) {
            if (errno == EINTR) continue;
            break;
        }
        fcntl(cli, F_SETFD, FD_CLOEXEC);

        char buf[4096];
        ssize_t n = read(cli, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "GET /api/rf/pair") == buf || strstr(buf, "POST /api/rf/pair") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    start_pairing_mode(slot);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pairing_started\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/stop-pair") == buf) {
                stop_pairing_mode();
                const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pairing_stopped\"}";
                write(cli, res, strlen(res));
            } else if (strstr(buf, "GET /api/rf/unpair") == buf || strstr(buf, "POST /api/rf/unpair") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : (strstr(buf, "slot=1") ? 1 : 0);
                    unpair_device(slot);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"unpaired\",\"ok\":true}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/pull-config") == buf || strstr(buf, "POST /api/rf/pull-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    rf_frame_t pull_frame;
                    memset(&pull_frame, 0, sizeof(pull_frame));
                    pull_frame.report_id = 0x20;
                    pull_frame.sub_id = 0x12;
                    pull_frame.dev_slot = (uint8_t)slot;
                    pull_frame.seq = g_tx_seq++;
                    pull_frame.hdr_flag = 0x50 | (pull_frame.seq & 0x0F);
                    pull_frame.msg_type = RF_MSG_CONFIG_PULL_REQ;
                    rf_send_frame(&pull_frame);
                    char res[128];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"pull_requested\",\"slot\":%d,\"ok\":true}", slot);
                    write(cli, res, rlen);
                }
            } else if (strstr(buf, "GET /api/rf/log-level") == buf || strstr(buf, "POST /api/rf/log-level") == buf) {
                char *lp = strstr(buf, "level=");
                if (lp) {
                    int lvl = atoi(lp + 6);
                    if (lvl >= 0 && lvl <= 3) {
                        g_log_level = lvl;
                        g_debug_logging = (g_log_level >= LOG_LEVEL_DEBUG);
                    }
                }
                char res[128];
                int rlen = snprintf(res, sizeof(res),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"log_level\":%d,\"ok\":true}", g_log_level);
                write(cli, res, rlen);
            } else if (strstr(buf, "GET /api/rf/send?hex=") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    char *hex = strstr(buf, "hex=") + 4;
                    char *end = strchr(hex, ' ');
                    if (end) *end = '\0';
                    uint8_t raw[64] = {0};
                    size_t raw_len = 0;
                    while (*hex && *(hex + 1) && raw_len < sizeof(raw)) {
                        unsigned int byte_val = 0;
                        char hex_byte[3] = { hex[0], hex[1], '\0' };
                        if (sscanf(hex_byte, "%02x", &byte_val) == 1 || sscanf(hex_byte, "%02X", &byte_val) == 1) {
                            raw[raw_len++] = (uint8_t)byte_val;
                            hex += 2;
                        } else break;
                    }
                    if (raw_len > 0) {
                        printf("[*] API RF TX (%zu bytes): ", raw_len);
                        for (size_t i = 0; i < raw_len; i++) printf("%02X ", raw[i]);
                        printf("\n");
                        rf_write_cmd(raw, raw_len);
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"sent\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/push-config") == buf || strstr(buf, "POST /api/rf/push-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    g_config_version++;
                    if (g_config_version == 0) g_config_version = 1;
                    FILE *vf = fopen("/data/codex/config_version", "w");
                    if (vf) { fprintf(vf, "%d\n", g_config_version); fclose(vf); }
                    pthread_t push_th;
                    pthread_create(&push_th, NULL, config_push_worker, NULL);
                    pthread_detach(push_th);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"push_started\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/sync") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"ota_in_progress\",\"ok\":true}";
                    write(cli, res, strlen(res));
                } else {
                    char cur[32] = "-1";
                    get_current_activity(cur, sizeof(cur));
                    rf_send_codex_activity_sync(cur);
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"synced\"}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/remote-config") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                char path[64];
                snprintf(path, sizeof(path), "/data/codex/elite_slot%d_config.json", slot);
                char res_body[4096] = {0};
                FILE *f = fopen(path, "r");
                if (f) {
                    size_t rn = fread(res_body, 1, sizeof(res_body) - 1, f);
                    res_body[rn] = '\0';
                    fclose(f);
                } else {
                    snprintf(res_body, sizeof(res_body),
                        "{\"slot\":%d,\"brightness\":3,\"timeout\":120,\"touchEnabled\":true,\"touch\":1,\"capButtonsEnabled\":true,\"cap_buttons\":1,\"gyroEnabled\":true,\"gyro\":1,\"gyroRate\":25,\"gyro_rate\":25,\"btnBacklight\":1,\"btn_backlight\":1,\"nightStart\":22,\"night_start\":22,\"nightEnd\":7,\"night_end\":7,\"navToggleKey\":139,\"nav_key\":139,\"navToggleMode\":1,\"nav_mode\":1,\"usbEth\":false,\"usbeth\":0,\"confirmActivity\":false,\"confirm_act\":0,\"touchSensitivity\":2,\"touch_sensitivity\":2,\"remoteMode\":0,\"remote_mode\":0,\"noScreen\":false,\"no_screen\":0}",
                        slot);
                }
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/remote-config") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    char *body_start = strstr(buf, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        cJSON *root = cJSON_Parse(body_start);
                        if (root) {
                            cJSON *jb = cJSON_GetObjectItem(root, "brightness");
                            cJSON *jt = cJSON_GetObjectItem(root, "timeout");
                            cJSON *jtouch = cJSON_GetObjectItem(root, "touchEnabled");
                            if (!jtouch) jtouch = cJSON_GetObjectItem(root, "touch");
                            cJSON *jgyro = cJSON_GetObjectItem(root, "gyroEnabled");
                            if (!jgyro) jgyro = cJSON_GetObjectItem(root, "gyro");
                            cJSON *jnk = cJSON_GetObjectItem(root, "navToggleKey");
                            if (!jnk) jnk = cJSON_GetObjectItem(root, "nav_key");
                            cJSON *jnm = cJSON_GetObjectItem(root, "navToggleMode");
                            if (!jnm) jnm = cJSON_GetObjectItem(root, "nav_mode");
                            cJSON *jca = cJSON_GetObjectItem(root, "confirmActivity");
                            if (!jca) jca = cJSON_GetObjectItem(root, "confirm_act");
                            cJSON *jts = cJSON_GetObjectItem(root, "touchSensitivity");
                            if (!jts) jts = cJSON_GetObjectItem(root, "touch_sensitivity");
                            cJSON *jrm = cJSON_GetObjectItem(root, "remoteMode");
                            if (!jrm) jrm = cJSON_GetObjectItem(root, "remote_mode");
                            cJSON *jgr = cJSON_GetObjectItem(root, "gyroRate");
                            if (!jgr) jgr = cJSON_GetObjectItem(root, "gyro_rate");
                            cJSON *jbb = cJSON_GetObjectItem(root, "btnBacklight");
                            if (!jbb) jbb = cJSON_GetObjectItem(root, "btn_backlight");
                            cJSON *jns = cJSON_GetObjectItem(root, "nightStart");
                            if (!jns) jns = cJSON_GetObjectItem(root, "night_start");
                            cJSON *jne = cJSON_GetObjectItem(root, "nightEnd");
                            if (!jne) jne = cJSON_GetObjectItem(root, "night_end");
                            cJSON *jue = cJSON_GetObjectItem(root, "usbEth");
                            if (!jue) jue = cJSON_GetObjectItem(root, "usbeth");
                            cJSON *jnscreen = cJSON_GetObjectItem(root, "noScreen");
                            if (!jnscreen) jnscreen = cJSON_GetObjectItem(root, "no_screen");
                            cJSON *jcap = cJSON_GetObjectItem(root, "capButtonsEnabled");
                            if (!jcap) jcap = cJSON_GetObjectItem(root, "cap_buttons");

                            int bright = jb ? jb->valueint : 3;
                            int timeout = jt ? jt->valueint : 120;
                            int touch = (jtouch && (cJSON_IsTrue(jtouch) || (cJSON_IsNumber(jtouch) && jtouch->valueint != 0))) ? 1 : 0;
                            int gyro = (jgyro && (cJSON_IsTrue(jgyro) || (cJSON_IsNumber(jgyro) && jgyro->valueint != 0))) ? 1 : (jgyro ? 0 : 1);
                            int nav_key = jnk ? jnk->valueint : 139;
                            int nav_mode = jnm ? jnm->valueint : 1;
                            int confirm_act = (jca && (cJSON_IsTrue(jca) || (cJSON_IsNumber(jca) && jca->valueint != 0))) ? 1 : 0;
                            int touch_sens = (jts && jts->valueint >= 1 && jts->valueint <= 3) ? jts->valueint : 2;
                            int remote_mode = (jrm && jrm->valueint == 1) ? 1 : 0;
                            int gyro_rate = (jgr && jgr->valueint > 0) ? jgr->valueint : 25;
                            int btn_backlight = (jbb && jbb->valueint >= 0 && jbb->valueint <= 2) ? jbb->valueint : 1;
                            int night_start = (jns && jns->valueint >= 0 && jns->valueint <= 23) ? jns->valueint : 22;
                            int night_end = (jne && jne->valueint >= 0 && jne->valueint <= 23) ? jne->valueint : 7;
                            int usbeth = (jue && (cJSON_IsTrue(jue) || (cJSON_IsNumber(jue) && jue->valueint != 0))) ? 1 : 0;
                            int no_screen = (jnscreen && (cJSON_IsTrue(jnscreen) || (cJSON_IsNumber(jnscreen) && jnscreen->valueint != 0))) ? 1 : 0;
                            int cap_buttons = (jcap && (cJSON_IsFalse(jcap) || (cJSON_IsNumber(jcap) && jcap->valueint == 0))) ? 0 : 1;

                            /* Create normalized JSON root saving all fields */
                            cJSON *norm = cJSON_CreateObject();
                            cJSON_AddNumberToObject(norm, "slot", slot);
                            cJSON_AddNumberToObject(norm, "brightness", bright);
                            cJSON_AddNumberToObject(norm, "timeout", timeout);
                            cJSON_AddBoolToObject(norm, "touchEnabled", touch != 0);
                            cJSON_AddNumberToObject(norm, "touch", touch);
                            cJSON_AddBoolToObject(norm, "capButtonsEnabled", cap_buttons != 0);
                            cJSON_AddNumberToObject(norm, "cap_buttons", cap_buttons);
                            cJSON_AddBoolToObject(norm, "gyroEnabled", gyro != 0);
                            cJSON_AddNumberToObject(norm, "gyro", gyro);
                            cJSON_AddNumberToObject(norm, "navToggleKey", nav_key);
                            cJSON_AddNumberToObject(norm, "nav_key", nav_key);
                            cJSON_AddNumberToObject(norm, "navToggleMode", nav_mode);
                            cJSON_AddNumberToObject(norm, "nav_mode", nav_mode);
                            cJSON_AddBoolToObject(norm, "confirmActivity", confirm_act != 0);
                            cJSON_AddNumberToObject(norm, "confirm_act", confirm_act);
                            cJSON_AddNumberToObject(norm, "touchSensitivity", touch_sens);
                            cJSON_AddNumberToObject(norm, "touch_sensitivity", touch_sens);
                            cJSON_AddNumberToObject(norm, "remoteMode", remote_mode);
                            cJSON_AddNumberToObject(norm, "remote_mode", remote_mode);
                            cJSON_AddNumberToObject(norm, "gyroRate", gyro_rate);
                            cJSON_AddNumberToObject(norm, "gyro_rate", gyro_rate);
                            cJSON_AddNumberToObject(norm, "btnBacklight", btn_backlight);
                            cJSON_AddNumberToObject(norm, "btn_backlight", btn_backlight);
                            cJSON_AddNumberToObject(norm, "nightStart", night_start);
                            cJSON_AddNumberToObject(norm, "night_start", night_start);
                            cJSON_AddNumberToObject(norm, "nightEnd", night_end);
                            cJSON_AddNumberToObject(norm, "night_end", night_end);
                            cJSON_AddBoolToObject(norm, "usbEth", usbeth != 0);
                            cJSON_AddNumberToObject(norm, "usbeth", usbeth);
                            cJSON_AddBoolToObject(norm, "noScreen", no_screen != 0);
                            cJSON_AddNumberToObject(norm, "no_screen", no_screen);

                            char path[64];
                            snprintf(path, sizeof(path), "/data/codex/elite_slot%d_config.json", slot);

                            /* Read existing config to compute diff for pending push banner */
                            char old_cfg[4096] = {0};
                            FILE *cur_f = fopen(path, "r");
                            if (cur_f) {
                                size_t cr = fread(old_cfg, 1, sizeof(old_cfg) - 1, cur_f);
                                old_cfg[cr] = '\0';
                                fclose(cur_f);
                            }
                            record_config_diff(slot, old_cfg, norm);

                            char *norm_str = cJSON_Print(norm);
                            cJSON_Delete(norm);
                            if (norm_str) {
                                FILE *f = fopen(path, "w");
                                if (f) {
                                    fputs(norm_str, f);
                                    fclose(f);
                                }
                                free(norm_str);
                            }
                            rf_send_settings_push_slot((uint8_t)slot, bright, timeout, touch, gyro, nav_key, nav_mode,
                                                       confirm_act, touch_sens, remote_mode, gyro_rate,
                                                       btn_backlight, night_start, night_end, usbeth, no_screen,
                                                       cap_buttons);
                            cJSON_Delete(root);
                        }
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"saved_and_pushed\",\"ok\":true}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/remote-direct-buttons") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                char path[64];
                snprintf(path, sizeof(path), "/data/codex/elite_slot%d_direct_buttons.json", slot);
                char res_body[4096] = {0};
                FILE *f = fopen(path, "r");
                if (f) {
                    size_t rn = fread(res_body, 1, sizeof(res_body) - 1, f);
                    res_body[rn] = '\0';
                    fclose(f);
                } else {
                    snprintf(res_body, sizeof(res_body), "{\"page1\":[],\"page2\":[],\"buttons\":{}}");
                }
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/remote-direct-buttons") == buf) {
                if (is_ota_busy()) {
                    const char *res = "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"error\":\"RF OTA in progress\",\"ok\":false}";
                    write(cli, res, strlen(res));
                } else {
                    int slot = strstr(buf, "slot=2") ? 2 : 1;
                    char *body_start = strstr(buf, "\r\n\r\n");
                    if (body_start) {
                        body_start += 4;
                        char path[64];
                        snprintf(path, sizeof(path), "/data/codex/elite_slot%d_direct_buttons.json", slot);
                        FILE *f = fopen(path, "w");
                        if (f) {
                            fputs(body_start, f);
                            fclose(f);
                        }
                        if (is_slot_direct_mode(slot)) {
                            pthread_t push_th;
                            pthread_create(&push_th, NULL, config_push_worker, NULL);
                            pthread_detach(push_th);
                        }
                    }
                    const char *res = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"saved\",\"ok\":true}";
                    write(cli, res, strlen(res));
                }
            } else if (strstr(buf, "GET /api/rf/ota-status") == buf) {
                pthread_mutex_lock(&g_ota_lock);
                char res_body[512];
                snprintf(res_body, sizeof(res_body),
                    "{\"in_progress\":%s,\"slot\":%d,\"status\":%d,\"sent_bytes\":%u,\"total_bytes\":%u,\"sent_chunks\":%u,\"total_chunks\":%u,\"error\":\"%s\"}",
                    g_rf_ota.in_progress ? "true" : "false",
                    g_rf_ota.target_slot,
                    g_rf_ota.status_code,
                    g_rf_ota.sent_bytes,
                    g_rf_ota.total_bytes,
                    g_rf_ota.sent_chunks,
                    g_rf_ota.total_chunks,
                    g_rf_ota.error_msg);
                pthread_mutex_unlock(&g_ota_lock);
                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", (int)strlen(res_body));
                write(cli, header, hlen);
                write(cli, res_body, strlen(res_body));
            } else if (strstr(buf, "POST /api/rf/ota-start") == buf) {
                int slot = strstr(buf, "slot=2") ? 2 : 1;
                int started = 0;
                int busy_slot = 0;
                pthread_mutex_lock(&g_ota_lock);
                if (!g_rf_ota.in_progress) {
                    g_rf_ota.in_progress = true;
                    g_rf_ota.target_slot = slot;
                    g_rf_ota.status_code = 1;
                    g_rf_ota.error_msg[0] = '\0';
                    pthread_t ota_th;
                    if (pthread_create(&ota_th, NULL, rf_ota_sender_worker, (void *)(uintptr_t)slot) == 0) {
                        pthread_detach(ota_th);
                        started = 1;
                    } else {
                        g_rf_ota.in_progress = false;
                        g_rf_ota.status_code = -1;
                    }
                } else {
                    busy_slot = g_rf_ota.target_slot;
                }
                pthread_mutex_unlock(&g_ota_lock);
                if (started) {
                    char res[128];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"started\",\"slot\":%d,\"ok\":true}", slot);
                    write(cli, res, rlen);
                } else {
                    char res[256];
                    int rlen = snprintf(res, sizeof(res),
                        "HTTP/1.1 409 Conflict\r\nContent-Type: application/json\r\nConnection: close\r\n\r\n{\"status\":\"already_in_progress\",\"slot\":%d,\"error\":\"RF OTA already in progress on slot %d\",\"ok\":false}", busy_slot, busy_slot);
                    write(cli, res, rlen);
                }
            } else {
                /* Status endpoint */
                char addr1_str[16] = "None";
                char addr2_str[16] = "None";
                if (g_paired_remotes[0].rf_addr[0] || g_paired_remotes[0].rf_addr[1] || g_paired_remotes[0].rf_addr[2] || g_paired_remotes[0].rf_addr[3]) {
                    snprintf(addr1_str, sizeof(addr1_str), "%02X%02X%02X%02X",
                             g_paired_remotes[0].rf_addr[0], g_paired_remotes[0].rf_addr[1], g_paired_remotes[0].rf_addr[2], g_paired_remotes[0].rf_addr[3]);
                }
                if (g_paired_remotes[1].rf_addr[0] || g_paired_remotes[1].rf_addr[1] || g_paired_remotes[1].rf_addr[2] || g_paired_remotes[1].rf_addr[3]) {
                    snprintf(addr2_str, sizeof(addr2_str), "%02X%02X%02X%02X",
                             g_paired_remotes[1].rf_addr[0], g_paired_remotes[1].rf_addr[1], g_paired_remotes[1].rf_addr[2], g_paired_remotes[1].rf_addr[3]);
                }

                char name1[32] = "Harmony Remote (Slot 1)";
                char name2[32] = "Harmony Elite (Slot 2)";
                if (g_paired_remotes[0].name[0]) snprintf(name1, sizeof(name1), "%s (Slot 1)", g_paired_remotes[0].name);
                if (g_paired_remotes[1].name[0]) snprintf(name2, sizeof(name2), "%s (Slot 2)", g_paired_remotes[1].name);

                if ((g_hub_rf_addr[0] | g_hub_rf_addr[1] | g_hub_rf_addr[2] | g_hub_rf_addr[3]) == 0) {
                    uint8_t q[7] = {0x10, 0xFF, 0x83, 0xB5, 0x03, 0x00, 0x00};
                    rf_write_cmd(q, sizeof(q));
                    usleep(30000);
                }

                char hub_rf_str[16] = "None";
                if (g_hub_rf_addr[0] || g_hub_rf_addr[1] || g_hub_rf_addr[2] || g_hub_rf_addr[3]) {
                    snprintf(hub_rf_str, sizeof(hub_rf_str), "%02X%02X%02X%02X",
                             g_hub_rf_addr[0], g_hub_rf_addr[1], g_hub_rf_addr[2], g_hub_rf_addr[3]);
                }

                bool sync_pending = (access("/data/codex/rf_sync_pending.flag", F_OK) == 0);
                int pending_slot = 1;
                char pending_diff[256] = "Hub configuration changed. Push settings to sync.";
                FILE *pdf = fopen("/data/codex/rf_sync_pending_details.json", "r");
                if (pdf) {
                    char dline[512] = {0};
                    if (fread(dline, 1, sizeof(dline) - 1, pdf) > 0) {
                        cJSON *pj = cJSON_Parse(dline);
                        if (pj) {
                            cJSON *ps = cJSON_GetObjectItem(pj, "slot");
                            cJSON *pd = cJSON_GetObjectItem(pj, "diff");
                            if (ps) pending_slot = ps->valueint;
                            if (pd && pd->valuestring) snprintf(pending_diff, sizeof(pending_diff), "%s", pd->valuestring);
                            cJSON_Delete(pj);
                        }
                    }
                    fclose(pdf);
                }

                bool slot1_in_sync = (g_remote_versions[0].config_version == g_config_version && g_paired_remotes[0].active);
                bool slot2_in_sync = (g_remote_versions[1].config_version == g_config_version && g_paired_remotes[1].active);

                pthread_mutex_lock(&g_ota_lock);
                bool ota_in_prog = g_rf_ota.in_progress;
                int ota_slot = g_rf_ota.target_slot;
                int ota_status = g_rf_ota.status_code;
                uint32_t ota_sent_b = g_rf_ota.sent_bytes;
                uint32_t ota_tot_b = g_rf_ota.total_bytes;
                uint16_t ota_sent_c = g_rf_ota.sent_chunks;
                uint16_t ota_tot_c = g_rf_ota.total_chunks;
                char ota_err[64];
                snprintf(ota_err, sizeof(ota_err), "%s", g_rf_ota.error_msg);
                pthread_mutex_unlock(&g_ota_lock);

                char body[3072];
                int blen = snprintf(body, sizeof(body),
                    "{\"running\":true,\"ok\":true,\"fw_version\":\"v%d.00\",\"paired\":%s,\"rf_address\":\"%s\",\"hub_rf_address\":\"%s\",\"pairing_active\":%s,"
                    "\"sync_pending\":%s,\"sync_pending_slot\":%d,\"sync_pending_diff\":\"%s\",\"hub_config_version\":%d,"
                    "\"ota\":{\"in_progress\":%s,\"slot\":%d,\"status\":%d,\"sent_bytes\":%u,\"total_bytes\":%u,\"sent_chunks\":%u,\"total_chunks\":%u,\"error\":\"%s\"},"
                    "\"slots\":["
                    "{\"slot\":1,\"name\":\"%s\",\"active\":%s,\"in_sync\":%s,\"address\":\"%s\",\"rf_address\":\"%s\",\"sw_version\":\"%s\",\"config_version\":%d,\"battery\":{\"percent\":%d,\"charging\":%s,\"voltage_mv\":%d}},"
                    "{\"slot\":2,\"name\":\"%s\",\"active\":%s,\"in_sync\":%s,\"address\":\"%s\",\"rf_address\":\"%s\",\"sw_version\":\"%s\",\"config_version\":%d,\"battery\":{\"percent\":%d,\"charging\":%s,\"voltage_mv\":%d}}"
                    "]}",
                    g_fw_version, (g_has_paired_device || g_paired_remotes[0].active || g_paired_remotes[1].active) ? "true" : "false",
                    hub_rf_str, hub_rf_str,
                    g_pairing_mode ? "true" : "false",
                    sync_pending ? "true" : "false",
                    pending_slot, pending_diff, g_config_version,
                    ota_in_prog ? "true" : "false", ota_slot, ota_status, ota_sent_b, ota_tot_b, ota_sent_c, ota_tot_c, ota_err,
                    name1, g_paired_remotes[0].active ? "true" : "false", slot1_in_sync ? "true" : "false", addr1_str, addr1_str,
                    g_remote_versions[0].sw_version, g_remote_versions[0].config_version,
                    g_remote_batteries[0].percent, g_remote_batteries[0].charging ? "true" : "false", g_remote_batteries[0].voltage_mv,
                    name2, g_paired_remotes[1].active ? "true" : "false", slot2_in_sync ? "true" : "false", addr2_str, addr2_str,
                    g_remote_versions[1].sw_version, g_remote_versions[1].config_version,
                    g_remote_batteries[1].percent, g_remote_batteries[1].charging ? "true" : "false", g_remote_batteries[1].voltage_mv);

                char header[256];
                int hlen = snprintf(header, sizeof(header),
                    "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", blen);
                write(cli, header, hlen);
                write(cli, body, blen);
            }
        }
        close(cli);
    }
    close(srv);
    return NULL;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Enforce single instance across the entire OS to protect AR9344 SPI arbiter */
    int lock_fd = open("/var/run/codex_rf.lock", O_CREAT | O_RDWR, 0644);
    if (lock_fd >= 0) {
        if (flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                fprintf(stderr, "[-] FATAL: Another codex_rf instance is already running! Exiting immediately to protect SPI arbiter.\n");
                return 1;
            }
            fprintf(stderr, "[-] Warning: flock failed: %s (ignoring non-contention error)\n", strerror(errno));
        }
    }

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    /* Load debug logging flag from persistent config */
    if (access("/data/codex/debug_logging.conf", F_OK) == 0) {
        g_debug_logging = 1;
        g_log_level = LOG_LEVEL_DEBUG;
    }

    printf("[+] Starting Harmony Hub RF Daemon (codex_rf)...\n");

    /* Open SPI driver */
    g_rf_fd = open(RFSPI_DEV, O_RDWR);
    if (g_rf_fd < 0) {
        fprintf(stderr, "[-] Warning: cannot open %s, attempting CC2544 flash...\n", RFSPI_DEV);
        flash_cc2544();
        g_rf_fd = open(RFSPI_DEV, O_RDWR);
        if (g_rf_fd < 0) {
            fprintf(stderr, "[-] Fatal: cannot open %s: %s\n", RFSPI_DEV, strerror(errno));
            return 1;
        }
    }
    printf("[+] Opened %s (fd=%d)\n", RFSPI_DEV, g_rf_fd);

    FILE *vf = fopen("/data/codex/config_version", "r");
    if (vf) {
        int v = 1;
        if (fscanf(vf, "%d", &v) == 1 && v > 0) g_config_version = (uint8_t)v;
        fclose(vf);
    } else {
        g_config_version = 1;
        FILE *wf = fopen("/data/codex/config_version", "w");
        if (wf) { fprintf(wf, "1\n"); fclose(wf); }
    }
    printf("[+] Codex RF config version: %d\n", g_config_version);

    /* Load last known remote software and config versions from disk */
    for (int slot = 1; slot <= 2; slot++) {
        char vpath[64];
        snprintf(vpath, sizeof(vpath), "/data/codex/elite_slot%d_version.json", slot);
        FILE *fv = fopen(vpath, "r");
        if (fv) {
            char vbuf[256] = {0};
            if (fread(vbuf, 1, sizeof(vbuf) - 1, fv) > 0) {
                cJSON *vroot = cJSON_Parse(vbuf);
                if (vroot) {
                    cJSON *jv = cJSON_GetObjectItem(vroot, "version");
                    cJSON *jcv = cJSON_GetObjectItem(vroot, "config_version");
                    if (jv && jv->valuestring) {
                        snprintf(g_remote_versions[slot - 1].sw_version, sizeof(g_remote_versions[slot - 1].sw_version), "%s", jv->valuestring);
                    }
                    if (jcv) {
                        g_remote_versions[slot - 1].config_version = (uint8_t)jcv->valueint;
                    }
                    cJSON_Delete(vroot);
                }
            }
            fclose(fv);
        }
    }

    /* 1. Initialize CC2544 message engine */
    uint8_t pkt_init[7] = {0x10, 0xFF, 0x80, 0x00, 0x00, 0x01, 0x00};
    rf_write_cmd(pkt_init, sizeof(pkt_init));
    usleep(50000);

    /* Close pairing lock to force normal operation with already-paired devices */
    uint8_t pkt_close_lock[7] = {0x10, 0xFF, 0x80, 0xB2, 0x02, 0x00, 0x00};
    rf_write_cmd(pkt_close_lock, sizeof(pkt_close_lock));
    usleep(50000);

    /* Set CC2544 active power mode (matches stock hal_hub hot_set_powermode 0x20) */
    uint8_t pkt_power[7] = {0x20, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00};
    rf_write_cmd(pkt_power, sizeof(pkt_power));
    usleep(50000);

    pthread_mutex_init(&g_rf_q_mutex, NULL);
    pthread_cond_init(&g_rf_q_cond, NULL);

    pthread_t rx_th;
    if (pthread_create(&rx_th, NULL, rx_worker, NULL) != 0) {
        perror("pthread_create rx_worker");
        close(g_rf_fd);
        return 1;
    }

    pthread_t disp_th;
    pthread_create(&disp_th, NULL, dispatch_worker, NULL);

    pthread_t sync_th;
    pthread_create(&sync_th, NULL, activity_sync_worker, NULL);

    /* 3. Query initial state */
    uint8_t pkt_ver[7] = {0x10, 0xFF, 0x81, 0xF1, 0x01, 0x00, 0x00};
    rf_write_cmd(pkt_ver, sizeof(pkt_ver));
    usleep(50000);

    query_paired_devices();

    /* 4. Start local REST API thread and Hub button listener */
    pthread_t api_th;
    pthread_create(&api_th, NULL, api_server_worker, NULL);
    printf("[+] RF API listening on http://127.0.0.1:%d/api/rf/...\n", RF_API_PORT);

    pthread_t btn_th;
    pthread_create(&btn_th, NULL, hub_button_worker, NULL);

    set_hub_led(1);

    /* Optional CLI flags */
    if (argc > 1) {
        if (strcmp(argv[1], "--pair") == 0) {
            int slot = (argc > 2) ? atoi(argv[2]) : 0;
            start_pairing_mode(slot);
        }
        else if (strcmp(argv[1], "--unpair") == 0) {
            int slot = (argc > 2) ? atoi(argv[2]) : 0;
            unpair_device(slot);
        }
    }

    printf("[+] codex_rf ready and operational.\n");

    /* Main loop wait */
    while (g_running) {
        sleep(1);
    }

    printf("[+] Shutting down codex_rf...\n");
    close(g_rf_fd);
    g_rf_fd = -1;
    pthread_cond_broadcast(&g_rf_q_cond);
    pthread_join(rx_th, NULL);
    pthread_join(disp_th, NULL);
    pthread_join(sync_th, NULL);
    return 0;
}
