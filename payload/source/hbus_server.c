/* Logitech protocol shim, see hbus_server.h. */
#define _GNU_SOURCE
#include "hbus_server.h"
#include "hbus_config.h"
#include "hub_ssdp.h"
#include "hub_info.h"
#include "http_server.h"
#include "ws_server.h"
#include "orchestrator.h"
#include "hw_action.h"
#include "sha256.h"
#include "harmony_version.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>

#define HBUS_PROVISION_FILE "/data/luaworks/provision/preferences"
#define HBUS_DISCOVERY_SERVER "https://svcs.myharmony.com/Discovery/Discovery.svc"
#define HBUS_REQ_MAX (64 * 1024)
#define HBUS_CHANNEL_GAP_MS 200

#define CMD_PREFIX "vnd.logitech.harmony/vnd.logitech.harmony.engine?"
#define CMD_CONFIG CMD_PREFIX "config"
#define CMD_CURRENT CMD_PREFIX "getCurrentActivity"
#define CMD_HOLD CMD_PREFIX "holdAction"
#define CMD_RUN "harmony.activityengine?runactivity"
#define CMD_START "harmony.engine?startActivity"
#define CMD_CHANNEL "harmony.engine?changeChannel"
#define CMD_SYNC "setup.sync?None"
#define CMD_DIGEST "vnd.logitech.connect/vnd.logitech.statedigest?get"
#define CMD_DISCOVERY "connect.discoveryinfo?get"
#define NOTIFY_DIGEST "connect.stateDigest?notify"
#define NOTIFY_FINISHED "harmony.engine?startActivityFinished"

static int g_port = HBUS_PORT;
static int g_listen_fd = -1;
static int g_initialized;
static struct hub_settings g_settings;
static int g_have_settings;
static char g_remote_id[32] = "";

/* configVersion: counts the inventory_version changes seen since start */
static char g_last_inventory[16] = "";
static int g_config_version = 0;

/* runactivity requests waiting for their completion frame, one per legacy client */
typedef struct {
    int fd;
    char id[80];
    char target[32];       /* the daemon's activity id */
} hbus_pending_t;
static hbus_pending_t g_pending[WS_MAX_CLIENTS];
static char g_announced_target[32] = "";

/* Clients that upgraded since the last tick: they get a digest notify (B.2) */
static int g_new_clients[WS_MAX_CLIENTS];
static int g_new_count;
static pthread_mutex_t g_new_lock = PTHREAD_MUTEX_INITIALIZER;

/* changeChannel digits in flight */
static char g_chan_device[32] = "";
static char g_chan_digits[24] = "";
static int g_chan_idx;
static uint64_t g_chan_next_ms;

static uint64_t now_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000u + (uint64_t)tv.tv_usec / 1000u;
}

/* ------------------------------------------------------------------------------------------ */
/* Remote id */

static void derive_remote_id(void) {
    const char *id = hub_info_id();
    uint8_t d[32];
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, (const uint8_t *)id, strlen(id));
    sha256_final(d, &ctx);
    uint32_t v = ((uint32_t)d[28] << 24 | (uint32_t)d[29] << 16 | (uint32_t)d[30] << 8 | d[31]) & 0x7FFFFFFFu;
    if (v == 0) v = 1;
    snprintf(g_remote_id, sizeof(g_remote_id), "%u", v);
}

static void load_remote_id(void) {
    g_remote_id[0] = '\0';
    FILE *f = fopen(HBUS_PROVISION_FILE, "r");
    if (f) {
        char buf[4096];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';
        cJSON *root = cJSON_Parse(buf);
        if (root) {
            const cJSON *r = cJSON_GetObjectItemCaseSensitive(root, "remoteId");
            if (r && cJSON_IsNumber(r) && r->valuedouble > 0) {
                snprintf(g_remote_id, sizeof(g_remote_id), "%ld", (long)r->valuedouble);
            } else if (r && cJSON_IsString(r) && r->valuestring && r->valuestring[0]) {
                snprintf(g_remote_id, sizeof(g_remote_id), "%s", r->valuestring);
            }
            cJSON_Delete(root);
        }
        if (g_remote_id[0]) printf("[+] HBUS: activeRemoteId %s from the stock provisioning file\n", g_remote_id);
    }
    if (!g_remote_id[0]) {
        derive_remote_id();
        printf("[+] HBUS: activeRemoteId %s derived from hub_id\n", g_remote_id);
    }
}

const char *hbus_remote_id(void) {
    if (!g_remote_id[0]) load_remote_id();
    return g_remote_id;
}

static const char *hub_name(void) {
    if (!g_have_settings) {
        config_store_get_settings(&g_settings);
        g_have_settings = 1;
    }
    return g_settings.hub_name[0] ? g_settings.hub_name : "Harmony Hub";
}

/* ------------------------------------------------------------------------------------------ */
/* Digest */

static int config_version_now(void) {
    char ver[9];
    hbus_inventory_version(ver);
    if (strcmp(ver, g_last_inventory) != 0) {
        snprintf(g_last_inventory, sizeof(g_last_inventory), "%s", ver);
        g_config_version++;
    }
    return g_config_version;
}

cJSON *hbus_digest(const char *activity_numeric, int activity_status) {
    cJSON *d = cJSON_CreateObject();
    if (!d) return NULL;
    const char *act = (activity_numeric && activity_numeric[0]) ? activity_numeric : "-1";
    cJSON_AddStringToObject(d, "activityId", act);
    cJSON_AddNumberToObject(d, "activityStatus", activity_status);
    cJSON_AddNumberToObject(d, "configVersion", config_version_now());
    cJSON_AddNumberToObject(d, "syncStatus", 0);
    cJSON_AddStringToObject(d, "hubSwVersion", HARMONY_VERSION);
    cJSON_AddStringToObject(d, "runningActivityList", strcmp(act, "-1") == 0 ? "" : act);
    cJSON_AddNumberToObject(d, "stateVersion", 1);
    cJSON_AddNumberToObject(d, "sleepTimerId", -1);
    cJSON_AddStringToObject(d, "tzOffset", "0");
    cJSON_AddStringToObject(d, "tzoffset", "0");
    cJSON_AddNumberToObject(d, "mode", 3);
    cJSON_AddNumberToObject(d, "wifiStatus", 1);
    cJSON_AddObjectToObject(d, "updates");
    cJSON_AddBoolToObject(d, "hubUpdate", 0);
    cJSON_AddNumberToObject(d, "contentVersion", 0);
    cJSON_AddStringToObject(d, "accountId", "0");
    cJSON_AddBoolToObject(d, "activitySetupState", 0);
    cJSON_AddStringToObject(d, "discoveryServer", HBUS_DISCOVERY_SERVER);
    cJSON_AddNumberToObject(d, "errorId", 0);
    cJSON_AddStringToObject(d, "errorCode", "200");
    cJSON_AddStringToObject(d, "errorString", "OK");
    cJSON_AddNumberToObject(d, "time", (double)time(NULL));
    cJSON_AddBoolToObject(d, "sequence", 0);
    return d;
}

/* The current activity as the stock client sees it: numeric id, "-1" when idle */
static void current_numeric(char *out, size_t cap) {
    const char *cur = orch_get_current_activity();
    if (!cur || !cur[0] || strcmp(cur, ORCH_POWEROFF_ID) == 0) snprintf(out, cap, "-1");
    else hbus_numeric_id(cur, out, cap);
}

static int current_status(void) {
    switch (orch_get_state()) {
        case ORCH_STATE_STARTING: return 1;
        case ORCH_STATE_RUNNING: return 2;
        case ORCH_STATE_STOPPING: return 3;
        default: return 0;
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Frames */

static void send_json(int fd, cJSON *obj) {
    if (!obj) return;
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) return;
    if (ws_send_text(fd, txt) != 0) ws_remove_client(fd);
    free(txt);
}

static void broadcast_json(cJSON *obj) {
    if (!obj) return;
    char *txt = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!txt) return;
    ws_broadcast_legacy_text(txt);
    free(txt);
}

/* {"cmd","id","code","msg","data"}; takes data (NULL = {}) */
static cJSON *reply(const char *cmd, const cJSON *id, int code, const char *msg, cJSON *data) {
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddStringToObject(r, "cmd", cmd ? cmd : "");
    if (id) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    cJSON_AddNumberToObject(r, "code", code);
    cJSON_AddStringToObject(r, "msg", msg ? msg : "OK");
    cJSON_AddItemToObject(r, "data", data ? data : cJSON_CreateObject());
    return r;
}

static cJSON *reply_id_str(const char *cmd, const char *id, int code, const char *msg, cJSON *data) {
    cJSON *idj = id ? cJSON_CreateString(id) : NULL;
    cJSON *r = reply(cmd, idj, code, msg, data);
    cJSON_Delete(idj);
    return r;
}

static cJSON *notify(const char *type, cJSON *data) {
    cJSON *n = cJSON_CreateObject();
    if (!n) {
        cJSON_Delete(data);
        return NULL;
    }
    cJSON_AddStringToObject(n, "type", type);
    cJSON_AddItemToObject(n, "data", data ? data : cJSON_CreateObject());
    return n;
}

static void broadcast_digest(const char *activity_numeric, int status) {
    broadcast_json(notify(NOTIFY_DIGEST, hbus_digest(activity_numeric, status)));
}

/* ------------------------------------------------------------------------------------------ */
/* Pending runactivity requests */

static hbus_pending_t *pending_for(int fd) {
    for (int i = 0; i < WS_MAX_CLIENTS; i++) {
        if (g_pending[i].fd == fd) return &g_pending[i];
    }
    return NULL;
}

static void pending_set(int fd, const char *id, const char *target) {
    hbus_pending_t *p = pending_for(fd);
    if (!p) p = pending_for(-1);
    if (!p) return;
    p->fd = fd;
    snprintf(p->id, sizeof(p->id), "%s", id ? id : "");
    snprintf(p->target, sizeof(p->target), "%s", target ? target : "");
}

static void pending_clear(hbus_pending_t *p) {
    p->fd = -1;
    p->id[0] = '\0';
    p->target[0] = '\0';
}

/* ------------------------------------------------------------------------------------------ */
/* WebSocket commands (main loop) */

static const char *param_str(const cJSON *params, const char *key, char *buf, size_t cap) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(params, key);
    if (j && cJSON_IsString(j) && j->valuestring) return j->valuestring;
    if (j && cJSON_IsNumber(j)) {
        snprintf(buf, cap, "%ld", (long)j->valuedouble);
        return buf;
    }
    return NULL;
}

static int activity_exists(const char *id) {
    if (strcmp(id, ORCH_POWEROFF_ID) == 0) return 1;
    cJSON *root = config_store_get_activities_json();
    cJSON *list = root ? (cJSON_IsArray(root) ? root : cJSON_GetObjectItemCaseSensitive(root, "activities")) : NULL;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, list) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(a, "id");
        char aid[32] = "";
        if (j && cJSON_IsString(j) && j->valuestring) snprintf(aid, sizeof(aid), "%s", j->valuestring);
        else if (j && cJSON_IsNumber(j)) snprintf(aid, sizeof(aid), "%ld", (long)j->valuedouble);
        if (strcmp(aid, id) == 0) return 1;
    }
    return 0;
}

static cJSON *cmd_runactivity(int fd, const char *cmd, const cJSON *id, const cJSON *params) {
    char nbuf[32];
    const char *num = param_str(params, "activityId", nbuf, sizeof(nbuf));
    if (!num || !num[0]) return reply(cmd, id, 500, "Unknown activity", NULL);
    char target[32];
    if (strcmp(num, "-1") == 0) snprintf(target, sizeof(target), "%s", ORCH_POWEROFF_ID);
    else hbus_resolve_id(num, target, sizeof(target));
    if (!activity_exists(target)) return reply(cmd, id, 500, "Unknown activity", NULL);
    if (orch_is_busy()) return reply(cmd, id, 500, "Busy", NULL);
    char idtxt[80] = "";
    if (id && cJSON_IsString(id) && id->valuestring) snprintf(idtxt, sizeof(idtxt), "%s", id->valuestring);
    else if (id && cJSON_IsNumber(id)) snprintf(idtxt, sizeof(idtxt), "%ld", (long)id->valuedouble);
    /* The request id must be remembered before the orchestrator emits (an empty transition
     * completes inside the call; the bus dispatches it on the next iteration) */
    pending_set(fd, idtxt, target);
    int rc = (strcmp(target, ORCH_POWEROFF_ID) == 0) ? orch_power_off(0, NULL, 0)
                                                     : orch_switch_activity(target, 0, NULL, 0);
    if (rc != 0) {
        hbus_pending_t *p = pending_for(fd);
        if (p) pending_clear(p);
        return reply(cmd, id, 500, rc == -2 ? "Busy" : "Unknown activity", NULL);
    }
    printf("[*] HBUS: runactivity %s (%s) from client fd %d\n", num, target, fd);
    return reply(cmd, id, 200, "OK", NULL);
}

/* holdAction's action is not JSON: every "::" stands for ":" */
static cJSON *parse_action(const char *action) {
    if (!action) return NULL;
    size_t n = strlen(action);
    char *fixed = (char *)malloc(n + 1);
    if (!fixed) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (action[i] == ':' && action[i + 1] == ':') {
            fixed[o++] = ':';
            i++;
        } else {
            fixed[o++] = action[i];
        }
    }
    fixed[o] = '\0';
    cJSON *obj = cJSON_Parse(fixed);
    free(fixed);
    return obj;
}

static cJSON *cmd_holdaction(const char *cmd, const cJSON *id, const cJSON *params) {
    char sbuf[16], abuf[16];
    const char *status = param_str(params, "status", sbuf, sizeof(sbuf));
    const char *action = param_str(params, "action", abuf, sizeof(abuf));
    cJSON *a = parse_action(action);
    if (!a) return reply(cmd, id, 500, "Bad action", NULL);
    char dbuf[32];
    const char *dev_num = param_str(a, "deviceId", dbuf, sizeof(dbuf));
    const cJSON *cj = cJSON_GetObjectItemCaseSensitive(a, "command");
    const char *command = (cj && cJSON_IsString(cj)) ? cj->valuestring : NULL;
    if (!dev_num || !command || !command[0]) {
        cJSON_Delete(a);
        return reply(cmd, id, 500, "Bad action", NULL);
    }
    char dev[32];
    hbus_resolve_id(dev_num, dev, sizeof(dev));
    int btn = (status && strcasecmp(status, "release") == 0) ? HW_BTN_ACTION_RELEASE : HW_BTN_ACTION_PRESS;
    int rc = hw_device_command_send_action(dev, 0, command, btn);
    cJSON_Delete(a);
    if (rc != 0) return reply(cmd, id, 500, "Command failed", NULL);
    return NULL; /* no reply on success */
}

/* The running activity's channel device: the first of its devices with a digit command */
static int channel_device(char *out, size_t cap) {
    const char *cur = orch_get_current_activity();
    if (!cur || !cur[0] || strcmp(cur, ORCH_POWEROFF_ID) == 0) return 0;
    cJSON *aroot = config_store_get_activities_json();
    cJSON *acts = aroot ? (cJSON_IsArray(aroot) ? aroot : cJSON_GetObjectItemCaseSensitive(aroot, "activities")) : NULL;
    cJSON *droot = config_store_get_devices_json();
    cJSON *devs = droot ? (cJSON_IsArray(droot) ? droot : cJSON_GetObjectItemCaseSensitive(droot, "devices")) : NULL;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        const cJSON *j = cJSON_GetObjectItemCaseSensitive(a, "id");
        char aid[32] = "";
        if (j && cJSON_IsString(j) && j->valuestring) snprintf(aid, sizeof(aid), "%s", j->valuestring);
        else if (j && cJSON_IsNumber(j)) snprintf(aid, sizeof(aid), "%ld", (long)j->valuedouble);
        if (strcmp(aid, cur) != 0) continue;
        const char *seqs[2] = { "startSequence", "stopSequence" };
        for (int s = 0; s < 2; s++) {
            const cJSON *st = NULL;
            cJSON_ArrayForEach(st, cJSON_GetObjectItemCaseSensitive(a, seqs[s])) {
                char did[32] = "";
                const cJSON *dj = cJSON_GetObjectItemCaseSensitive(st, "deviceId");
                if (dj && cJSON_IsString(dj) && dj->valuestring) snprintf(did, sizeof(did), "%s", dj->valuestring);
                else if (dj && cJSON_IsNumber(dj)) snprintf(did, sizeof(did), "%ld", (long)dj->valuedouble);
                if (!did[0]) continue;
                cJSON *d = NULL;
                cJSON_ArrayForEach(d, devs) {
                    const cJSON *ij = cJSON_GetObjectItemCaseSensitive(d, "id");
                    char xid[32] = "";
                    if (ij && cJSON_IsString(ij) && ij->valuestring) snprintf(xid, sizeof(xid), "%s", ij->valuestring);
                    else if (ij && cJSON_IsNumber(ij)) snprintf(xid, sizeof(xid), "%ld", (long)ij->valuedouble);
                    if (strcmp(xid, did) != 0) continue;
                    const cJSON *c = NULL;
                    cJSON_ArrayForEach(c, cJSON_GetObjectItemCaseSensitive(d, "commands")) {
                        const char *nm = cJSON_IsString(c) ? c->valuestring : NULL;
                        if (!nm) {
                            const cJSON *nj = cJSON_GetObjectItemCaseSensitive(c, "name");
                            nm = (nj && cJSON_IsString(nj)) ? nj->valuestring : NULL;
                        }
                        if (nm && strlen(nm) == 1 && nm[0] >= '0' && nm[0] <= '9') {
                            snprintf(out, cap, "%s", did);
                            return 1;
                        }
                    }
                }
            }
        }
        return 0;
    }
    return 0;
}

static cJSON *cmd_changechannel(const char *cmd, const cJSON *id, const cJSON *params) {
    char cbuf[24];
    const char *channel = param_str(params, "channel", cbuf, sizeof(cbuf));
    if (!channel || !channel[0]) return reply(cmd, id, 500, "No channel", NULL);
    for (const char *p = channel; *p; p++) {
        if (*p < '0' || *p > '9') return reply(cmd, id, 500, "Bad channel", NULL);
    }
    if (g_chan_digits[g_chan_idx]) return reply(cmd, id, 500, "Busy", NULL);
    char dev[32];
    if (!channel_device(dev, sizeof(dev))) return reply(cmd, id, 500, "No channel device", NULL);
    snprintf(g_chan_device, sizeof(g_chan_device), "%s", dev);
    snprintf(g_chan_digits, sizeof(g_chan_digits), "%s", channel);
    g_chan_idx = 0;
    g_chan_next_ms = now_ms();
    return reply(cmd, id, 200, "OK", NULL);
}

static cJSON *cmd_discoveryinfo(const char *cmd, const cJSON *id) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "friendlyName", hub_name());
    cJSON_AddStringToObject(d, "remoteId", hbus_remote_id());
    cJSON_AddStringToObject(d, "hubId", hub_info_id());
    cJSON_AddStringToObject(d, "current_fw_version", HARMONY_VERSION);
    cJSON_AddStringToObject(d, "protocolVersion", "1.0");
    return reply(cmd, id, 200, "OK", d);
}

void hbus_handle_ws_message(int cfd) {
    char *buf = (char *)malloc(HBUS_REQ_MAX);
    if (!buf) return;
    int opcode = 0;
    int len = ws_read_frame(cfd, buf, HBUS_REQ_MAX, &opcode);
    if (len < 0 || opcode == WS_OP_CLOSE) {
        hbus_pending_t *p = pending_for(cfd);
        if (p) pending_clear(p);
        ws_remove_client(cfd);
        free(buf);
        return;
    }
    if (opcode == WS_OP_PING) {
        ws_send_pong(cfd);
        free(buf);
        return;
    }
    if (opcode != WS_OP_TEXT || len == 0) {
        free(buf);
        return;
    }
    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return;
    const cJSON *hbus = cJSON_GetObjectItemCaseSensitive(root, "hbus");
    if (!hbus || !cJSON_IsObject(hbus)) {
        cJSON_Delete(root);
        return;
    }
    const cJSON *cj = cJSON_GetObjectItemCaseSensitive(hbus, "cmd");
    const char *cmd = (cj && cJSON_IsString(cj) && cj->valuestring) ? cj->valuestring : "";
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(hbus, "id");
    const cJSON *params = cJSON_GetObjectItemCaseSensitive(hbus, "params");

    cJSON *resp = NULL;
    if (strcmp(cmd, CMD_CONFIG) == 0) {
        char ver[9];
        hbus_inventory_version(ver);
        resp = reply(cmd, id, 200, "OK", hbus_build_config(hbus_remote_id(), ver));
    } else if (strcmp(cmd, CMD_CURRENT) == 0) {
        char cur[32];
        current_numeric(cur, sizeof(cur));
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "result", cur);
        resp = reply(cmd, id, 200, "OK", d);
    } else if (strcmp(cmd, CMD_RUN) == 0) {
        resp = cmd_runactivity(cfd, cmd, id, params);
    } else if (strcmp(cmd, CMD_HOLD) == 0) {
        resp = cmd_holdaction(cmd, id, params);
    } else if (strcmp(cmd, CMD_CHANNEL) == 0) {
        resp = cmd_changechannel(cmd, id, params);
    } else if (strcmp(cmd, CMD_SYNC) == 0) {
        resp = reply(cmd, id, 200, "OK", NULL);
    } else if (strcmp(cmd, CMD_DIGEST) == 0) {
        char cur[32];
        current_numeric(cur, sizeof(cur));
        resp = reply(cmd, id, 200, "OK", hbus_digest(cur, current_status()));
    } else if (strcmp(cmd, CMD_DISCOVERY) == 0) {
        resp = cmd_discoveryinfo(cmd, id);
    } else {
        resp = reply(cmd, id, 404, "Not Found", NULL);
    }
    cJSON_Delete(root);
    if (resp) send_json(cfd, resp);
}

/* ------------------------------------------------------------------------------------------ */
/* Bus sink */

void hbus_event_sink(const hub_event_t *ev, void *user) {
    (void)user;
    if (!ev || !ev->payload || g_listen_fd < 0) return;
    const cJSON *p = ev->payload;
    if (ev->type == HUB_EV_ACTIVITY_PROGRESS) {
        const cJSON *aj = cJSON_GetObjectItemCaseSensitive(p, "activityId");
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(p, "stateCode");
        const cJSON *step = cJSON_GetObjectItemCaseSensitive(p, "step");
        const cJSON *total = cJSON_GetObjectItemCaseSensitive(p, "total");
        const char *target = (aj && cJSON_IsString(aj)) ? aj->valuestring : ORCH_POWEROFF_ID;
        int state = (sj && cJSON_IsNumber(sj)) ? sj->valueint : 0;
        if (state != ORCH_STATE_STARTING && state != ORCH_STATE_STOPPING) return;
        char num[32];
        if (strcmp(target, ORCH_POWEROFF_ID) == 0) snprintf(num, sizeof(num), "-1");
        else hbus_numeric_id(target, num, sizeof(num));
        /* Step 2: the transition's first progress announces the target */
        if (strcmp(g_announced_target, target) != 0) {
            snprintf(g_announced_target, sizeof(g_announced_target), "%s", target);
            broadcast_digest(num, state == ORCH_STATE_STARTING ? 1 : 0);
        }
        /* Step 3: progress to the requester */
        int done = (step && cJSON_IsNumber(step)) ? step->valueint : 0;
        int tot = (total && cJSON_IsNumber(total)) ? total->valueint : 0;
        if (tot <= 0) return;
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (g_pending[i].fd < 0 || strcmp(g_pending[i].target, target) != 0) continue;
            cJSON *d = cJSON_CreateObject();
            cJSON_AddNumberToObject(d, "done", done);
            cJSON_AddNumberToObject(d, "total", tot);
            send_json(g_pending[i].fd, reply_id_str(CMD_START, g_pending[i].id, 100, "OK", d));
        }
        return;
    }
    if (ev->type == HUB_EV_ACTIVITY_STATE) {
        const cJSON *sj = cJSON_GetObjectItemCaseSensitive(p, "state");
        const cJSON *aj = cJSON_GetObjectItemCaseSensitive(p, "activity");
        int state = (sj && cJSON_IsNumber(sj)) ? sj->valueint : 0;
        if (state != ORCH_STATE_RUNNING && state != ORCH_STATE_IDLE) return;
        const char *cur = (aj && cJSON_IsString(aj)) ? aj->valuestring : ORCH_POWEROFF_ID;
        char num[32];
        if (strcmp(cur, ORCH_POWEROFF_ID) == 0) snprintf(num, sizeof(num), "-1");
        else hbus_numeric_id(cur, num, sizeof(num));
        g_announced_target[0] = '\0';
        /* Step 4: completion to the requester, once */
        for (int i = 0; i < WS_MAX_CLIENTS; i++) {
            if (g_pending[i].fd < 0 || strcmp(g_pending[i].target, cur) != 0) continue;
            send_json(g_pending[i].fd, reply_id_str(CMD_START, g_pending[i].id, 200, "OK", NULL));
            pending_clear(&g_pending[i]);
        }
        /* Step 5: finished + digest to every legacy client */
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "activityId", num);
        cJSON_AddStringToObject(d, "errorCode", "200");
        cJSON_AddStringToObject(d, "errorString", "OK");
        broadcast_json(notify(NOTIFY_FINISHED, d));
        broadcast_digest(num, state == ORCH_STATE_RUNNING ? 2 : 0);
        return;
    }
    if (ev->type == HUB_EV_CONFIG_CHANGED) {
        /* A new configVersion makes the clients refetch the configuration */
        char cur[32];
        current_numeric(cur, sizeof(cur));
        broadcast_digest(cur, current_status());
    }
}

/* ------------------------------------------------------------------------------------------ */
/* Main loop tick */

void hbus_tick(void) {
    int fds[WS_MAX_CLIENTS];
    int n = 0;
    pthread_mutex_lock(&g_new_lock);
    n = g_new_count;
    memcpy(fds, g_new_clients, sizeof(int) * (size_t)n);
    g_new_count = 0;
    pthread_mutex_unlock(&g_new_lock);
    for (int i = 0; i < n; i++) {
        char cur[32];
        current_numeric(cur, sizeof(cur));
        send_json(fds[i], notify(NOTIFY_DIGEST, hbus_digest(cur, current_status())));
    }
    if (g_chan_digits[g_chan_idx] && now_ms() >= g_chan_next_ms) {
        char digit[2] = { g_chan_digits[g_chan_idx], '\0' };
        hw_device_command_send_action(g_chan_device, 0, digit, HW_BTN_ACTION_TAP);
        g_chan_idx++;
        g_chan_next_ms = now_ms() + HBUS_CHANNEL_GAP_MS;
        if (!g_chan_digits[g_chan_idx]) {
            g_chan_digits[0] = '\0';
            g_chan_idx = 0;
        }
    }
}

/* ------------------------------------------------------------------------------------------ */
/* HTTP side (worker thread) */

static void send_http(int fd, int code, const char *reason, const char *ctype, const char *body) {
    size_t blen = body ? strlen(body) : 0;
    char hdr[256];
    int n = snprintf(hdr, sizeof(hdr),
                     "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     code, reason, ctype, blen);
    send(fd, hdr, (size_t)n, MSG_NOSIGNAL);
    if (blen) send(fd, body, blen, MSG_NOSIGNAL);
}

static cJSON *provision_reply(const cJSON *req) {
    cJSON *r = cJSON_CreateObject();
    const cJSON *id = req ? cJSON_GetObjectItemCaseSensitive(req, "id ") : NULL;
    if (!id && req) id = cJSON_GetObjectItemCaseSensitive(req, "id");
    if (id && (cJSON_IsNumber(id) || cJSON_IsString(id))) cJSON_AddItemToObject(r, "id", cJSON_Duplicate(id, 1));
    else cJSON_AddNumberToObject(r, "id", 1);
    cJSON_AddNumberToObject(r, "code", 200);
    cJSON_AddStringToObject(r, "msg", "OK");
    cJSON *d = cJSON_AddObjectToObject(r, "data");
    cJSON_AddStringToObject(d, "activeRemoteId", hbus_remote_id());
    cJSON_AddStringToObject(d, "hubSwVersion", HARMONY_VERSION);
    cJSON_AddStringToObject(d, "discoveryServer", HBUS_DISCOVERY_SERVER);
    cJSON_AddStringToObject(d, "hubName", hub_name());
    cJSON_AddStringToObject(d, "accountId", "0");
    cJSON_AddStringToObject(d, "email", "");
    return r;
}

void hbus_handle_client(int cfd) {
    struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
    setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    char *buf = (char *)calloc(1, HBUS_REQ_MAX + 1);
    if (!buf) {
        close(cfd);
        return;
    }
    size_t total = 0;
    char *hdr_end = NULL;
    while (total < HBUS_REQ_MAX) {
        ssize_t n = recv(cfd, buf + total, HBUS_REQ_MAX - total, 0);
        if (n <= 0) break;
        total += (size_t)n;
        buf[total] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
        if (hdr_end) break;
    }
    if (total == 0 || !hdr_end) {
        /* aioharmony's TCP probe: a connect with no data, closed by the client */
        free(buf);
        close(cfd);
        return;
    }
    char method[16] = {0}, path[256] = {0};
    if (sscanf(buf, "%15s %255s", method, path) != 2) {
        send_http(cfd, 400, "Bad Request", "text/plain", "");
        free(buf);
        close(cfd);
        return;
    }
    char route[256];
    snprintf(route, sizeof(route), "%s", path);
    char *q = strchr(route, '?');
    if (q) *q = '\0';

    if (ws_is_upgrade_req(buf)) {
        if (strcmp(route, "/") != 0) {
            send_http(cfd, 404, "Not Found", "text/plain", "");
            close(cfd);
        } else if (ws_handle_upgrade(cfd, buf) != 0) {
            close(cfd);
        } else {
            ws_set_client_legacy(cfd, 1);
            pthread_mutex_lock(&g_new_lock);
            if (g_new_count < WS_MAX_CLIENTS) g_new_clients[g_new_count++] = cfd;
            pthread_mutex_unlock(&g_new_lock);
            printf("[+] HBUS: legacy client connected (fd %d)\n", cfd);
        }
        free(buf);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(route, "/") == 0) {
        size_t hdr_len = (size_t)(hdr_end + 4 - buf);
        long cl = 0;
        char *clp = strcasestr(buf, "Content-Length:");
        if (clp && clp < hdr_end) cl = atol(clp + 15);
        if (cl > 0 && (size_t)cl < HBUS_REQ_MAX - hdr_len) {
            while (total < hdr_len + (size_t)cl) {
                ssize_t n = recv(cfd, buf + total, hdr_len + (size_t)cl - total, 0);
                if (n <= 0) break;
                total += (size_t)n;
            }
            buf[total] = '\0';
        }
        cJSON *req = cJSON_Parse(buf + hdr_len);
        cJSON *r = provision_reply(req);
        cJSON_Delete(req);
        char *txt = cJSON_PrintUnformatted(r);
        cJSON_Delete(r);
        send_http(cfd, 200, "OK", "application/json", txt ? txt : "{}");
        free(txt);
    } else if (strcmp(method, "GET") == 0 && strcmp(route, "/description") == 0) {
        char *xml = hub_ssdp_description_xml();
        send_http(cfd, 200, "OK", "text/xml; charset=utf-8", xml ? xml : "");
        free(xml);
    } else {
        send_http(cfd, 404, "Not Found", "text/plain", "");
    }
    free(buf);
    close(cfd);
}

/* ------------------------------------------------------------------------------------------ */
/* Lifecycle */

static void start(void) {
    if (g_listen_fd >= 0) return;
    g_listen_fd = http_server_init(g_port);
    if (g_listen_fd < 0) {
        fprintf(stderr, "[-] HBUS: could not bind port %d: %s\n", g_port, strerror(errno));
        return;
    }
    printf("[+] HBUS: Logitech protocol shim listening on port %d (no auth)\n", g_port);
}

static void stop(void) {
    if (g_listen_fd < 0) return;
    http_server_close(g_listen_fd);
    g_listen_fd = -1;
    ws_close_legacy_clients();
    for (int i = 0; i < WS_MAX_CLIENTS; i++) pending_clear(&g_pending[i]);
    pthread_mutex_lock(&g_new_lock);
    g_new_count = 0;
    pthread_mutex_unlock(&g_new_lock);
    printf("[*] HBUS: shim stopped\n");
}

int hbus_init(int port) {
    g_port = port > 0 ? port : HBUS_PORT;
    for (int i = 0; i < WS_MAX_CLIENTS; i++) pending_clear(&g_pending[i]);
    hbus_remote_id();
    if (!g_have_settings) {
        config_store_get_settings(&g_settings);
        g_have_settings = 1;
    }
    g_initialized = 1;
    if (g_settings.legacy_enabled) start();
    return (g_settings.legacy_enabled && g_listen_fd < 0) ? -1 : 0;
}

int hbus_listen_fd(void) {
    return g_listen_fd;
}

void hbus_apply_settings(const struct hub_settings *s) {
    if (!s) return;
    g_settings = *s;
    g_have_settings = 1;
    if (!g_initialized) return;
    if (s->legacy_enabled) start();
    else stop();
}

void hbus_shutdown(void) {
    stop();
}
