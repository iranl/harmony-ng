#define _GNU_SOURCE
#include "config_sanitize.h"
#include "config_store.h"
#include "cJSON.h"
#include "bt_common.h"
#include "json_number.h"
#include "plugin_mgr.h"
#include "cmd_range.h"
#include "cmd_ir_alt.h"
#include "net_device.h"
#include "ir_encoder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

/* Names end up in HTML (both web UIs, Home Assistant cards, other clients): keep '<', '>'
 * and control characters out of them, so a name can never become markup. Every other
 * character is kept. Returns out. */
static const char *clean_display_name(const char *in, char *out, size_t out_sz) {
    size_t j = 0;
    if (!out || out_sz == 0) return "";
    for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && j + 1 < out_sz; p++) {
        if (*p == '<' || *p == '>' || *p < 0x20 || *p == 0x7f) continue;
        out[j++] = (char)*p;
    }
    out[j] = '\0';
    return out;
}

/* ---- HTTP and TCP devices (net_device.h) ---- */

static const char *net_str(const cJSON *o, const char *key) {
    const cJSON *j = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(j) && j->valuestring ? j->valuestring : NULL;
}

/* Text without control characters (newlines too when !multiline), at most max bytes */
static int net_text_ok(const char *s, size_t max, int multiline) {
    if (!s || strlen(s) > max) return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (*p == 0x7f) return 0;
        if (*p < 0x20 && !(*p == '\t' || (multiline && (*p == '\n' || *p == '\r')))) return 0;
    }
    return 1;
}

/* Host name, IPv4 or IPv6 address: letters, digits, '.', '-', '_', ':'. Not the Hub itself
 * (loopback: a device there would let a device test call the Hub's own API as admin), and not
 * starting with '-' (hub_fetch would read it as an option). */
int net_host_valid(const char *h) {
    size_t n = h ? strlen(h) : 0;
    if (n == 0 || n > 253 || h[0] == '-') return 0;
    if (strcasecmp(h, "localhost") == 0 || strcasecmp(h, "localhost.") == 0 || strncmp(h, "127.", 4) == 0 ||
            strcmp(h, "::1") == 0 || strcmp(h, "0.0.0.0") == 0 || strcmp(h, "::") == 0) {
#ifdef TEST_HARNESS
        /* the host tests run their devices on 127.0.0.1 and name their ports in
         * HARMONY_TEST_NET_LOOPBACK_PORTS; net_action.c checks the port of every send */
        const char *tp = getenv("HARMONY_TEST_NET_LOOPBACK_PORTS");
        if (!(tp && tp[0] && strcmp(h, "127.0.0.1") == 0))
#endif
        return 0;
    }
    for (const char *p = h; *p; p++) {
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '-' && *p != '_' && *p != ':') return 0;
    }
    return 1;
}

/* An HTTP header name (RFC 9110 token); the client sets Host, Connection, Content-Length */
int net_header_name_ok(const char *n) {
    size_t len = n ? strlen(n) : 0;
    if (len == 0 || len > 64) return 0;
    if (strcasecmp(n, "Host") == 0 || strcasecmp(n, "Connection") == 0 ||
            strcasecmp(n, "Content-Length") == 0 || strcasecmp(n, "Transfer-Encoding") == 0) return 0;
    for (const char *p = n; *p; p++) {
        if (!isalnum((unsigned char)*p) && !strchr("!#$%&'*+-.^_`|~", *p)) return 0;
    }
    return 1;
}

static void sanitize_net_device(const cJSON *src, cJSON *cdev, int kind) {
    const char *host = net_str(src, "netHost");
    cJSON_AddStringToObject(cdev, "netHost", net_host_valid(host) ? host : "");

    const cJSON *jtls = cJSON_GetObjectItemCaseSensitive(src, "netTls");
    int tls = cJSON_IsTrue(jtls) || (cJSON_IsNumber(jtls) && jtls->valueint == 1);
    cJSON_AddBoolToObject(cdev, "netTls", tls);

    const cJSON *jport = cJSON_GetObjectItemCaseSensitive(src, "netPort");
    int port = cJSON_IsNumber(jport) ? json_number_int(jport, 0, 65535, 0) : 0;
    if (port <= 0 && kind == NET_KIND_HTTP) port = tls ? 443 : 80;
    cJSON_AddNumberToObject(cdev, "netPort", port > 0 ? port : 0);

    const char *verify = net_str(src, "netTlsVerify");
    if (!verify || (strcmp(verify, "pin") != 0 && strcmp(verify, "insecure") != 0)) verify = "verify";
    cJSON_AddStringToObject(cdev, "netTlsVerify", verify);

    const cJSON *jto = cJSON_GetObjectItemCaseSensitive(src, "netTimeoutMs");
    int to = cJSON_IsNumber(jto) ? json_number_int(jto, 0, NET_TIMEOUT_MAX_MS, NET_TIMEOUT_DEFAULT_MS)
                                 : NET_TIMEOUT_DEFAULT_MS;
    if (to < NET_TIMEOUT_MIN_MS) to = NET_TIMEOUT_MIN_MS;
    cJSON_AddNumberToObject(cdev, "netTimeoutMs", to);

    if (kind != NET_KIND_HTTP) return;

    cJSON *hdrs = cJSON_AddArrayToObject(cdev, "httpHeaders");
    const cJSON *sh = cJSON_GetObjectItemCaseSensitive(src, "httpHeaders");
    const cJSON *h = NULL;
    int nh = 0;
    cJSON_ArrayForEach(h, sh) {
        const char *hn = net_str(h, "name"), *hv = net_str(h, "value");
        if (nh >= NET_MAX_HEADERS || !net_header_name_ok(hn) || !net_text_ok(hv ? hv : "", 1024, 0)) continue;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", hn);
        cJSON_AddStringToObject(o, "value", hv ? hv : "");
        cJSON_AddItemToArray(hdrs, o);
        nh++;
    }

    const char *auth = net_str(src, "httpAuthType");
    if (!auth || (strcmp(auth, "basic") != 0 && strcmp(auth, "bearer") != 0)) auth = "none";
    cJSON_AddStringToObject(cdev, "httpAuthType", auth);
    const char *user = net_str(src, "httpUser");
    const char *pass = net_str(src, "httpPassword");
    const char *token = net_str(src, "httpToken");
    /* Credentials of the other auth types are dropped: only the chosen one is stored */
    if (strcmp(auth, "basic") == 0) {
        cJSON_AddStringToObject(cdev, "httpUser", net_text_ok(user, 128, 0) && !strchr(user, ':') ? user : "");
        cJSON_AddStringToObject(cdev, "httpPassword", net_text_ok(pass, 256, 0) ? pass : "");
    } else if (strcmp(auth, "bearer") == 0) {
        cJSON_AddStringToObject(cdev, "httpToken", net_text_ok(token, 1024, 0) ? token : "");
    }
}

static void set_string(cJSON *o, const char *key, const char *val) {
    if (cJSON_GetObjectItemCaseSensitive(o, key))
        cJSON_ReplaceItemInObjectCaseSensitive(o, key, cJSON_CreateString(val));
    else
        cJSON_AddStringToObject(o, key, val);
}

static int hex_payload_ok(const char *s) {
    int digits = 0;
    for (const char *p = s; *p; p++) {
        if (isxdigit((unsigned char)*p)) digits++;
        else if (*p != ' ' && *p != ':') return 0;
    }
    return digits > 0 && digits % 2 == 0;
}

static void sanitize_net_command(const cJSON *src, cJSON *ccmd, int kind, const char *name) {
    char kc[160];
    snprintf(kc, sizeof(kc), "%s:%s", kind == NET_KIND_HTTP ? "HTTP" : "TCP", name);
    set_string(ccmd, "keyCode", kc);
    set_string(ccmd, "raw", kc);
    if (kind == NET_KIND_HTTP) {
        static const char *const methods[] = { "GET", "POST", "PUT", "PATCH", "DELETE", "HEAD", NULL };
        const char *m = net_str(src, "httpMethod");
        const char *method = "GET";
        for (int i = 0; m && methods[i]; i++) {
            if (strcasecmp(m, methods[i]) == 0) method = methods[i];
        }
        cJSON_AddStringToObject(ccmd, "httpMethod", method);
        const char *path = net_str(src, "httpPath");
        int path_ok = path && path[0] == '/' && net_text_ok(path, 1024, 0) && !strchr(path, ' ');
        cJSON_AddStringToObject(ccmd, "httpPath", path_ok ? path : "/");
        const char *body = net_str(src, "httpBody");
        cJSON_AddStringToObject(ccmd, "httpBody", net_text_ok(body, NET_BODY_MAX, 1) ? body : "");
        const char *ct = net_str(src, "httpContentType");
        cJSON_AddStringToObject(ccmd, "httpContentType", net_text_ok(ct, 128, 0) ? ct : "");
    } else {
        const char *fmt = net_str(src, "tcpFormat");
        int hex = fmt && strcmp(fmt, "hex") == 0;
        const char *pl = net_str(src, "tcpPayload");
        int ok = hex ? (pl && strlen(pl) <= NET_BODY_MAX * 3 && hex_payload_ok(pl))
                     : net_text_ok(pl, NET_BODY_MAX, 1);
        cJSON_AddStringToObject(ccmd, "tcpFormat", hex ? "hex" : "text");
        cJSON_AddStringToObject(ccmd, "tcpPayload", ok ? pl : "");
    }
}

/* GET /api/devices and /api/inventory: stored secrets as "" plus has* flags. Returns the
 * number of devices changed (0: nothing to hide, the caller can send the original). */
int config_mask_device_secrets(cJSON *root) {
    int changed = 0;
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        static const char *const keys[][2] = { { "httpPassword", "hasHttpPassword" },
                                               { "httpToken", "hasHttpToken" } };
        for (int i = 0; i < 2; i++) {
            const char *v = net_str(d, keys[i][0]);
            if (!v) continue;
            int has = v[0] != '\0';
            set_string(d, keys[i][0], "");
            cJSON_AddBoolToObject(d, keys[i][1], has);
            changed++;
        }
    }
    return changed;
}

int config_devices_have_secrets(const cJSON *root) {
    const cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "devices") : NULL;
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, devs) {
        if (net_str(d, "httpPassword") || net_str(d, "httpToken")) return 1;
    }
    return 0;
}

/* Sanitize devices JSON:
 * 1. Ensure canonical {"devices": [...]} structure
 * 2. Normalize and validate fields to clean camelCase canonical schema
 * 3. Normalize commands and power sequences
 */
_Static_assert(PRESS_MIN_REPEATS_MAX == IR_PRESS_MIN_REPEATS_MAX, "the sender's limit of pressMinRepeats");

int press_min_repeats_value(const cJSON *j, int *out) {
    double v;
    if (!j || cJSON_IsNull(j)) return 0;
    if (cJSON_IsNumber(j)) {
        v = j->valuedouble;
    } else if (cJSON_IsString(j) && j->valuestring) {
        const char *t = j->valuestring;
        while (isspace((unsigned char)*t)) t++;
        if (!*t) return 0;
        char *end = NULL;
        long l = strtol(t, &end, 10);
        while (end && isspace((unsigned char)*end)) end++;
        if (!end || end == t || *end) return -1;
        v = (double)l;
    } else {
        return -1;
    }
    if (v != v) return -1; /* NaN */
    if (v < 0) return 0;
    if (out) *out = v > PRESS_MIN_REPEATS_MAX ? PRESS_MIN_REPEATS_MAX : (int)v;
    return 1;
}

void sanitize_devices_json(cJSON *root) {
    if (!root) return;

    cJSON *devs = cJSON_GetObjectItemCaseSensitive(root, "devices");
    if (!devs || !cJSON_IsArray(devs)) return;

    /* Before commandPorts 1 every IR command was given its device's controlPort of that time,
     * and no send read it. A command's port is now used before the device's: those copies are
     * dropped once, so a later Blaster Assignment still reaches every command. */
    cJSON *jcp_ver = cJSON_GetObjectItemCaseSensitive(root, "commandPorts");
    int own_ports = cJSON_IsNumber(jcp_ver) && jcp_ver->valuedouble >= 1;
    if (!own_ports) {
        cJSON_DeleteItemFromObjectCaseSensitive(root, "commandPorts");
        cJSON_AddNumberToObject(root, "commandPorts", 1);
    }

    cJSON *clean_devs = cJSON_CreateArray();

    cJSON *src_d = NULL;
    cJSON_ArrayForEach(src_d, devs) {
        if (!src_d || !cJSON_IsObject(src_d)) continue;

        cJSON *jid = cJSON_GetObjectItemCaseSensitive(src_d, "id");
        char id_str[64] = {0};
        if (jid && cJSON_IsNumber(jid)) json_number_text(jid, id_str, sizeof(id_str));
        else if (jid && cJSON_IsString(jid) && jid->valuestring) strncpy(id_str, jid->valuestring, sizeof(id_str) - 1);
        if (!id_str[0]) {
            /* not addressable without an id; say so, the next save no longer has it */
            cJSON *jn = cJSON_GetObjectItemCaseSensitive(src_d, "name");
            fprintf(stderr, "[!] Config: device without an id left out (name \"%s\")\n",
                    cJSON_IsString(jn) && jn->valuestring ? jn->valuestring : "");
            continue;
        }
        {
            /* every lookup takes the first device of an id: a second one cannot be reached */
            cJSON *seen = NULL;
            cJSON_ArrayForEach(seen, clean_devs) {
                const char *sid = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(seen, "id"));
                if (sid && strcmp(sid, id_str) == 0) {
                    fprintf(stderr, "[!] Config: device id \"%s\" is used twice: only the first is used\n", id_str);
                    break;
                }
            }
        }

        cJSON *cdev = cJSON_CreateObject();
        cJSON_AddStringToObject(cdev, "id", id_str);

        cJSON *jname = cJSON_GetObjectItemCaseSensitive(src_d, "name");
        char name_buf[256];
        const char *name_str = (jname && jname->valuestring && jname->valuestring[0]) ? jname->valuestring : "Device";
        name_str = clean_display_name(name_str, name_buf, sizeof(name_buf));
        cJSON_AddStringToObject(cdev, "name", name_str[0] ? name_str : "Device");

        cJSON *jmfg = cJSON_GetObjectItemCaseSensitive(src_d, "manufacturer");
        char mfg_buf[256];
        const char *mfg_str = clean_display_name((jmfg && jmfg->valuestring) ? jmfg->valuestring : "", mfg_buf, sizeof(mfg_buf));
        cJSON_AddStringToObject(cdev, "manufacturer", mfg_str);

        cJSON *jmdl = cJSON_GetObjectItemCaseSensitive(src_d, "model");
        char mdl_buf[256];
        const char *mdl_str = clean_display_name((jmdl && jmdl->valuestring) ? jmdl->valuestring : "", mdl_buf, sizeof(mdl_buf));
        cJSON_AddStringToObject(cdev, "model", mdl_str);

        cJSON *jtype = cJSON_GetObjectItemCaseSensitive(src_d, "type");
        const char *type_str = (jtype && jtype->valuestring) ? jtype->valuestring : "IR";

        /* A plugin device only through its "plugin" field or a "plugin:<id>" type, with a safe
         * id: the free-text type of an IR device is never compared with plugin names. That
         * choice wins over the MQTT and Bluetooth guesses below: a plugin whose name says
         * "Bluetooth" (the new UI puts it in the manufacturer) lost its plugin. */
        char res_plug_id[64] = {0};
        int is_plugin = plugin_mgr_device_plugin_id(src_d, res_plug_id, sizeof(res_plug_id));

        /* HTTP and TCP only by their exact type names; they win over the MQTT and
         * Bluetooth guesses (a device named "MQTT bridge" that is driven over HTTP) */
        int net_kind = is_plugin ? NET_KIND_NONE : net_device_kind(src_d);

        cJSON *jmqtt_en = cJSON_GetObjectItemCaseSensitive(src_d, "mqttEnabled");
        int is_mqtt = !is_plugin && !net_kind && (strcasecmp(type_str, "MQTT") == 0 ||
                       (jmqtt_en && (cJSON_IsTrue(jmqtt_en) || (cJSON_IsNumber(jmqtt_en) && jmqtt_en->valueint == 1))));

        cJSON *jtrans = cJSON_GetObjectItemCaseSensitive(src_d, "transport");
        int is_bt = !is_plugin && !net_kind && !is_mqtt && ((jtrans && cJSON_IsNumber(jtrans) && jtrans->valueint == 32) ||
                       strcasecmp(type_str, "BT") == 0 || strcasecmp(type_str, "Bluetooth") == 0 ||
                       strcasestr(type_str, "bluetooth") != NULL || strcasestr(mfg_str, "bluetooth") != NULL ||
                       strcasecmp(type_str, "BTHID") == 0);

        const char *final_type = "IR";
        char plug_type_buf[128] = {0};
        if (net_kind) {
            final_type = net_kind == NET_KIND_HTTP ? "HTTP" : "TCP";
        } else if (is_mqtt) {
            final_type = (type_str && type_str[0]) ? type_str : "MQTT";
        } else if (is_bt) {
            /* A device that is Bluetooth only by "transport": 32 must say so in its type: the
             * output has no transport field, and the next pass would take it for IR and drop its
             * Bluetooth settings */
            int type_says_bt = strcasecmp(type_str, "BT") == 0 || strcasestr(type_str, "bluetooth") != NULL ||
                               strcasecmp(type_str, "BTHID") == 0 || strcasestr(mfg_str, "bluetooth") != NULL;
            final_type = (type_str && type_str[0] && type_says_bt) ? type_str : "BT";
        } else if (is_plugin) {
            if (res_plug_id[0]) {
                if (type_str && type_str[0] && strncasecmp(type_str, "plugin:", 7) != 0 && strcasecmp(type_str, "plugin") != 0) {
                    final_type = type_str; /* preserve custom device type, e.g. Roku, PS5 */
                } else {
                    snprintf(plug_type_buf, sizeof(plug_type_buf), "plugin:%s", res_plug_id);
                    final_type = plug_type_buf;
                }
            } else {
                final_type = (type_str && type_str[0]) ? type_str : "plugin";
            }
        } else if (type_str && type_str[0]) {
            final_type = type_str;
        }
        cJSON_AddStringToObject(cdev, "type", final_type);

        cJSON *jicon = cJSON_GetObjectItemCaseSensitive(src_d, "icon");
        if (jicon) {
            if (cJSON_IsString(jicon) && jicon->valuestring) cJSON_AddStringToObject(cdev, "icon", jicon->valuestring);
            else if (cJSON_IsNumber(jicon)) cJSON_AddNumberToObject(cdev, "icon", jicon->valueint);
        }

        if (!is_bt && !is_mqtt && !is_plugin && !net_kind) {
            cJSON *jcp = cJSON_GetObjectItemCaseSensitive(src_d, "controlPort");
            int cp_val = (jcp && cJSON_IsNumber(jcp)) ? jcp->valueint : 7;
            cJSON_AddNumberToObject(cdev, "controlPort", (cp_val >= 0 && cp_val <= 7) ? cp_val : 7);
        }

        /* Timings */
        cJSON *jidd = cJSON_GetObjectItemCaseSensitive(src_d, "interDeviceDelay");
        int idd = (jidd && cJSON_IsNumber(jidd)) ? jidd->valueint : (jidd && cJSON_IsString(jidd) ? atoi(jidd->valuestring) : 500);
        if (idd > DEVICE_DELAY_MAX_MS) idd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "interDeviceDelay", idd >= 0 ? idd : 500);

        cJSON *jikd = cJSON_GetObjectItemCaseSensitive(src_d, "interKeyDelay");
        int ikd = (jikd && cJSON_IsNumber(jikd)) ? jikd->valueint : (jikd && cJSON_IsString(jikd) ? atoi(jikd->valuestring) : 100);
        if (ikd > DEVICE_DELAY_MAX_MS) ikd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "interKeyDelay", ikd >= 0 ? ikd : 100);

        cJSON *jhikd = cJSON_GetObjectItemCaseSensitive(src_d, "holdInterKeyDelay");
        int hikd = (jhikd && cJSON_IsNumber(jhikd)) ? jhikd->valueint : (jhikd && cJSON_IsString(jhikd) ? atoi(jhikd->valuestring) : 200);
        if (hikd > DEVICE_DELAY_MAX_MS) hikd = DEVICE_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "holdInterKeyDelay", hikd >= 0 ? hikd : 200);

        cJSON *jdidd = cJSON_GetObjectItemCaseSensitive(src_d, "defaultInterDeviceDelay");
        int didd = (jdidd && cJSON_IsNumber(jdidd)) ? json_number_int(jdidd, -1, DEVICE_DELAY_MAX_MS, 500) : 500;
        cJSON_AddNumberToObject(cdev, "defaultInterDeviceDelay", didd >= 0 ? didd : 500);

        cJSON *jpod = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnDelay");
        int pod = (jpod && cJSON_IsNumber(jpod)) ? json_number_int(jpod, -1, POWER_ON_DELAY_MAX_MS, 1500)
                                                 : (jpod && cJSON_IsString(jpod) ? atoi(jpod->valuestring) : 1500);
        if (pod > POWER_ON_DELAY_MAX_MS) pod = POWER_ON_DELAY_MAX_MS;
        cJSON_AddNumberToObject(cdev, "powerOnDelay", pod >= 0 ? pod : 1500);

        cJSON *jpao = cJSON_GetObjectItemCaseSensitive(src_d, "isPowerAlwaysOn");
        int pao = (jpao && (cJSON_IsTrue(jpao) || (cJSON_IsNumber(jpao) && jpao->valuedouble > 0) ||
                   (cJSON_IsString(jpao) && (strcmp(jpao->valuestring, "1") == 0 || strcasecmp(jpao->valuestring, "true") == 0))));
        cJSON_AddBoolToObject(cdev, "isPowerAlwaysOn", pao ? 1 : 0);

        /* Left out of the remote's device lists; its commands still work from activity keys */
        cJSON *jhide = cJSON_GetObjectItemCaseSensitive(src_d, "hideOnRemote");
        if (cJSON_IsTrue(jhide) || (cJSON_IsNumber(jhide) && jhide->valuedouble > 0))
            cJSON_AddTrueToObject(cdev, "hideOnRemote");

        /* Frames an infrared press sends after the first: kept only when set (no default
         * written, the Hub's default applies without it) */
        int pmr = 0;
        if (press_min_repeats_value(cJSON_GetObjectItemCaseSensitive(src_d, "pressMinRepeats"), &pmr) == 1)
            cJSON_AddNumberToObject(cdev, "pressMinRepeats", pmr);

        /* Left out of the web UIs' Devices tab only (not the remote) */
        cJSON *jhtab = cJSON_GetObjectItemCaseSensitive(src_d, "hideInDevicesTab");
        if (cJSON_IsTrue(jhtab) || (cJSON_IsNumber(jhtab) && jhtab->valuedouble > 0))
            cJSON_AddTrueToObject(cdev, "hideInDevicesTab");

        /* The device's Bluetooth host: bdaddr, else the older btHost key. The rebuilt object
         * keeps only bdaddr, so a btHost-only device would otherwise lose its host on save. */
        const char *host_addr = NULL;
        {
            const char *keys[] = {"bdaddr", "btHost", NULL};
            for (int k = 0; keys[k] && !host_addr; k++) {
                cJSON *jh = cJSON_GetObjectItemCaseSensitive(src_d, keys[k]);
                if (jh && cJSON_IsString(jh) && jh->valuestring && jh->valuestring[0]) host_addr = jh->valuestring;
            }
        }

        /* A device of another transport can still be linked to a Bluetooth host (a TV driven by
         * IR that pairs as a keyboard host): keep its address, it names the host and is the
         * only place the link lives (POST /api/bt-link-device). Keys still go by type. */
        if (!is_bt && host_addr) {
            cJSON_AddStringToObject(cdev, "bdaddr", host_addr);
        }

        /* BT Reconnect */
        if (is_bt) {
            cJSON_AddStringToObject(cdev, "bdaddr", host_addr ? host_addr : "");

            cJSON *jrec = cJSON_GetObjectItemCaseSensitive(src_d, "btAutoReconnect");
            int rec = (jrec && cJSON_IsNumber(jrec)) ? jrec->valueint : (jrec && cJSON_IsBool(jrec) ? (cJSON_IsTrue(jrec) ? 1 : 0) : 1);
            cJSON_AddNumberToObject(cdev, "btAutoReconnect", rec);

            cJSON *jrdur = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDurationSec");
            cJSON_AddNumberToObject(cdev, "btReconnectDurationSec", (jrdur && cJSON_IsNumber(jrdur)) ? jrdur->valueint : 0);

            cJSON *jrdel = cJSON_GetObjectItemCaseSensitive(src_d, "btReconnectDelaySec");
            cJSON_AddNumberToObject(cdev, "btReconnectDelaySec", (jrdel && cJSON_IsNumber(jrdel)) ? jrdel->valueint : -1);

            cJSON *j_btd = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerDeviceId");
            if (j_btd && cJSON_IsString(j_btd) && j_btd->valuestring) {
                cJSON_AddStringToObject(cdev, "btTriggerDeviceId", j_btd->valuestring);
            }
            cJSON *j_btc = cJSON_GetObjectItemCaseSensitive(src_d, "btTriggerCommandName");
            if (j_btc && cJSON_IsString(j_btc) && j_btc->valuestring) {
                cJSON_AddStringToObject(cdev, "btTriggerCommandName", j_btc->valuestring);
            }
        }

        /* MQTT */
        if (is_mqtt) {
            cJSON_AddBoolToObject(cdev, "mqttEnabled", 1);
            cJSON *jtop = cJSON_GetObjectItemCaseSensitive(src_d, "mqttTopic");
            cJSON_AddStringToObject(cdev, "mqttTopic", (jtop && jtop->valuestring && jtop->valuestring[0]) ?
                                    jtop->valuestring : "{base_topic}/button/{device}/{command}");
            cJSON *jpul = cJSON_GetObjectItemCaseSensitive(src_d, "mqttPulseMs");
            cJSON_AddNumberToObject(cdev, "mqttPulseMs", (jpul && cJSON_IsNumber(jpul)) ? jpul->valueint : 1000);
        }

        if (net_kind) sanitize_net_device(src_d, cdev, net_kind);

        /* Plugin */
        if (res_plug_id[0]) {
            cJSON_AddStringToObject(cdev, "plugin", res_plug_id);
        }

        cJSON *j_cfg = cJSON_GetObjectItemCaseSensitive(src_d, "config");
        if (j_cfg && cJSON_IsObject(j_cfg)) {
            cJSON_AddItemToObject(cdev, "config", cJSON_Duplicate(j_cfg, 1));
        } else if (j_cfg && cJSON_IsString(j_cfg) && j_cfg->valuestring && j_cfg->valuestring[0]) {
            cJSON *parsed = cJSON_Parse(j_cfg->valuestring);
            if (parsed && cJSON_IsObject(parsed)) {
                cJSON_AddItemToObject(cdev, "config", parsed);
            } else if (parsed) {
                cJSON_Delete(parsed);
            }
        }

        /* Power Sequences */
        cJSON *po = cJSON_GetObjectItemCaseSensitive(src_d, "powerOnSequence");
        if (po && cJSON_IsArray(po)) {
            cJSON *po_dup = cJSON_Duplicate(po, 1);
            sanitize_activity_steps(po_dup);
            cJSON_AddItemToObject(cdev, "powerOnSequence", po_dup);
        } else {
            cJSON_AddArrayToObject(cdev, "powerOnSequence");
        }

        cJSON *pf = cJSON_GetObjectItemCaseSensitive(src_d, "powerOffSequence");
        if (pf && cJSON_IsArray(pf)) {
            cJSON *pf_dup = cJSON_Duplicate(pf, 1);
            sanitize_activity_steps(pf_dup);
            cJSON_AddItemToObject(cdev, "powerOffSequence", pf_dup);
        } else {
            cJSON_AddArrayToObject(cdev, "powerOffSequence");
        }

        /* Commands */
        cJSON *clean_cmds = cJSON_AddArrayToObject(cdev, "commands");
        cJSON *cmds = cJSON_GetObjectItemCaseSensitive(src_d, "commands");
        int has_connect = 0, has_disconnect = 0;

        if (cmds && cJSON_IsArray(cmds)) {
            cJSON *c = NULL;
            cJSON_ArrayForEach(c, cmds) {
                cJSON *cname = cJSON_GetObjectItemCaseSensitive(c, "name");
                if (!cname || !cname->valuestring || !cname->valuestring[0]) continue;

                if (strcasecmp(cname->valuestring, "Connect") == 0) has_connect = 1;
                if (strcasecmp(cname->valuestring, "Disconnect") == 0) has_disconnect = 1;

                char cmd_name[128];
                clean_display_name(cname->valuestring, cmd_name, sizeof(cmd_name));
                if (!cmd_name[0]) continue;
                cJSON *ccmd = cJSON_CreateObject();
                cJSON_AddStringToObject(ccmd, "name", cmd_name);

                cJSON *ckc = cJSON_GetObjectItemCaseSensitive(c, "keyCode");
                const char *kc_str = (ckc && ckc->valuestring) ? ckc->valuestring : "";

                cJSON *craw = cJSON_GetObjectItemCaseSensitive(c, "raw");
                const char *raw_str = (craw && craw->valuestring) ? craw->valuestring : "";

                const char *raw_val = (raw_str && raw_str[0]) ? raw_str : kc_str;
                const char *kc_val = (kc_str && kc_str[0]) ? kc_str : raw_val;
                cJSON_AddStringToObject(ccmd, "keyCode", kc_val);
                cJSON_AddStringToObject(ccmd, "raw", raw_val);

                /* The infrared alternative of a plugin device's command (cmd_ir_alt.h): irKeyCode and
                 * irWhen, each kept only when valid; with an irKeyCode the command keeps its IR
                 * fields (port, repeats) like a command of an IR device */
                int ir_alt = 0;
                if (is_plugin) {
                    cJSON *cikc = cJSON_GetObjectItemCaseSensitive(c, "irKeyCode");
                    cJSON *ciw = cJSON_GetObjectItemCaseSensitive(c, "irWhen");
                    if (cmd_ir_keycode_ok(cikc)) {
                        cJSON_AddStringToObject(ccmd, "irKeyCode", cikc->valuestring);
                        ir_alt = 1;
                    }
                    if (cmd_ir_when_ok(ciw)) cJSON_AddStringToObject(ccmd, "irWhen", ciw->valuestring);
                }

                if (ir_alt || (!is_bt && !is_mqtt && !is_plugin && !net_kind)) {
                    /* a command's own port mask 1..7 (Emitter Port); none: the device's controlPort */
                    cJSON *cp_item = cJSON_GetObjectItemCaseSensitive(c, "port");
                    int port_val = (own_ports && cp_item && cJSON_IsNumber(cp_item)) ? json_number_int(cp_item, -1, 8, 0) : 0;
                    if (port_val >= 1 && port_val <= 7) cJSON_AddNumberToObject(ccmd, "port", port_val);

                    cJSON *crep = cJSON_GetObjectItemCaseSensitive(c, "repeats");
                    int rep_val = (crep && cJSON_IsNumber(crep)) ? json_number_int(crep, -1, 256, 3) : 3;
                    if (rep_val < 0 || rep_val > 255) rep_val = 3;
                    cJSON_AddNumberToObject(ccmd, "repeats", rep_val);
                }

                cJSON *cmacro = cJSON_GetObjectItemCaseSensitive(c, "isMacro");
                if (cmacro && cJSON_IsTrue(cmacro)) cJSON_AddBoolToObject(ccmd, "isMacro", 1);
                cmd_range_copy(ccmd, c);
                if (net_kind && !(cmacro && cJSON_IsTrue(cmacro))) sanitize_net_command(c, ccmd, net_kind, cmd_name);

                cJSON_AddItemToArray(clean_cmds, ccmd);
            }
        }

        if (is_bt) {
            if (!has_connect) {
                cJSON *cc = cJSON_CreateObject();
                cJSON_AddStringToObject(cc, "name", "Connect");
                cJSON_AddStringToObject(cc, "keyCode", "CONNECT");
                cJSON_AddStringToObject(cc, "raw", "CONNECT");
                cJSON_AddItemToArray(clean_cmds, cc);
            }
            if (!has_disconnect) {
                cJSON *dc = cJSON_CreateObject();
                cJSON_AddStringToObject(dc, "name", "Disconnect");
                cJSON_AddStringToObject(dc, "keyCode", "DISCONNECT");
                cJSON_AddStringToObject(dc, "raw", "DISCONNECT");
                cJSON_AddItemToArray(clean_cmds, dc);
            }
        }

        cJSON_AddItemToArray(clean_devs, cdev);
    }

    /* Replace elements in devs with clean_devs */
    while (cJSON_GetArraySize(devs) > 0) {
        cJSON_DeleteItemFromArray(devs, 0);
    }
    while (cJSON_GetArraySize(clean_devs) > 0) {
        cJSON *item = cJSON_DetachItemFromArray(clean_devs, 0);
        cJSON_AddItemToArray(devs, item);
    }
    cJSON_Delete(clean_devs);
}

/* Sanitize activity step arrays (PowerOn/PowerOff sequences, startSequence, stopSequence):
 * 1. Validate fields against canonical schema (type, command, deviceId, delay, order)
 * 2. Prune invalid steps without command or delay
 * 3. Normalize step types to 'Command', 'Delay', 'Switched', 'BTCommand'
 */
void sanitize_activity_steps(cJSON *steps) {
    if (!steps || !cJSON_IsArray(steps)) return;

    cJSON *new_arr = cJSON_CreateArray();
    cJSON *s = NULL;
    cJSON_ArrayForEach(s, steps) {
        if (!s || !cJSON_IsObject(s)) continue;

        /* cJSON_GetStringValue() is NULL for a missing key and for any type but a string
         * (null, a number, a bool...), whose valuestring is NULL too */
        const char *tstr = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "type"));
        if (!tstr) tstr = "";

        const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(s, "command"));
        if (!cmd) cmd = "";

        cJSON *dev_item = cJSON_GetObjectItemCaseSensitive(s, "deviceId");
        const char *dev_id = cJSON_GetStringValue(dev_item);
        char dev_num[32];
        if (!dev_id && cJSON_IsNumber(dev_item)) {
            /* A numeric device id is a valid id: keep it instead of dropping it to "0" */
            if (json_number_text(dev_item, dev_num, sizeof(dev_num))) dev_id = dev_num;
        }
        if (!dev_id) dev_id = "";

        cJSON *delay_item = cJSON_GetObjectItemCaseSensitive(s, "delay");
        int delay_val = 0;
        if (delay_item) {
            if (cJSON_IsNumber(delay_item)) delay_val = json_number_int(delay_item, -1, STEP_DELAY_MAX_MS, 0);
            else if (cJSON_IsString(delay_item) && delay_item->valuestring) delay_val = atoi(delay_item->valuestring);
        }
        if (delay_val > STEP_DELAY_MAX_MS) delay_val = STEP_DELAY_MAX_MS;

        /* Check for Switched pseudo-step */
        if (strcasecmp(tstr, "Switched") == 0 || strcasecmp(cmd, "Switched") == 0) {
            cJSON *clean_step = cJSON_CreateObject();
            cJSON_AddStringToObject(clean_step, "type", "Switched");
            cJSON_AddStringToObject(clean_step, "command", "Switched");
            cJSON_AddStringToObject(clean_step, "deviceId", "0");
            cJSON_AddNumberToObject(clean_step, "delay", 0);
            cJSON_AddItemToArray(new_arr, clean_step);
            continue;
        }

        /* Check for delay step */
        if (strcasecmp(tstr, "Delay") == 0 || (delay_val > 0 && (!cmd || !cmd[0]))) {
            cJSON *clean_step = cJSON_CreateObject();
            cJSON_AddStringToObject(clean_step, "type", "Delay");
            cJSON_AddStringToObject(clean_step, "command", "");
            cJSON_AddStringToObject(clean_step, "deviceId", "0");
            cJSON_AddNumberToObject(clean_step, "delay", delay_val > 0 ? delay_val : 1000);
            cJSON_AddItemToArray(new_arr, clean_step);
            continue;
        }

        /* Check for BT or IR command step */
        if (cmd && cmd[0]) {
            cJSON *clean_step = cJSON_CreateObject();
            if (strcasecmp(tstr, "BTCommand") == 0 || strncmp(cmd, "HID:", 4) == 0 || strncmp(cmd, "BT:", 3) == 0) {
                cJSON_AddStringToObject(clean_step, "type", "BTCommand");
            } else {
                cJSON_AddStringToObject(clean_step, "type", "Command");
            }
            cJSON_AddStringToObject(clean_step, "command", cmd);
            cJSON_AddStringToObject(clean_step, "deviceId", (dev_id && dev_id[0]) ? dev_id : "0");
            if (delay_val > 0) {
                cJSON_AddNumberToObject(clean_step, "delay", delay_val);
            }
            cJSON_AddItemToArray(new_arr, clean_step);
        }
    }

    /* Replace elements in steps with new_arr */
    while (cJSON_GetArraySize(steps) > 0) {
        cJSON_DeleteItemFromArray(steps, 0);
    }
    while (cJSON_GetArraySize(new_arr) > 0) {
        cJSON *item = cJSON_DetachItemFromArray(new_arr, 0);
        cJSON_AddItemToArray(steps, item);
    }
    cJSON_Delete(new_arr);
}

/* The activity's Display Order; one without a number goes after all numbered ones */
static double activity_order(const cJSON *a) {
    const cJSON *o = cJSON_IsObject(a) ? cJSON_GetObjectItemCaseSensitive(a, "order") : NULL;
    return cJSON_IsNumber(o) ? o->valuedouble : 1e18;
}

/* Every reader (both web UIs, the remotes' "yours" order, hbus, MQTT) lists the activities in
 * array order, so the array is kept sorted by Display Order. Stable: equal orders keep their
 * place. */
static int sort_activities_by_order(cJSON *acts) {
    int n = cJSON_GetArraySize(acts);
    if (n < 2) return 0;
    cJSON **items = malloc((size_t)n * sizeof(*items));
    if (!items) return 0;
    int k = 0;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) items[k++] = a;
    int moved = 0;
    for (int i = 1; i < n; i++) {
        cJSON *cur = items[i];
        double ord = activity_order(cur);
        int j = i - 1;
        while (j >= 0 && activity_order(items[j]) > ord) {
            items[j + 1] = items[j];
            j--;
            moved = 1;
        }
        items[j + 1] = cur;
    }
    if (moved) {
        for (int i = 0; i < n; i++) cJSON_DetachItemViaPointer(acts, items[i]);
        for (int i = 0; i < n; i++) cJSON_AddItemToArray(acts, items[i]);
    }
    free(items);
    return moved;
}

/* An activity without a Display Order gets its own, after the highest one (99 at most, as the
 * editor's field), so both web UIs show the number the Hub sorts by. Returns how many got one. */
static int number_unordered_activities(cJSON *acts) {
    double max = 0;
    int missing = 0;
    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        if (!cJSON_IsObject(a)) continue;
        const cJSON *o = cJSON_GetObjectItemCaseSensitive(a, "order");
        if (cJSON_IsNumber(o)) {
            if (o->valuedouble > max) max = o->valuedouble;
        } else {
            missing++;
        }
    }
    if (!missing) return 0;
    cJSON_ArrayForEach(a, acts) {
        if (!cJSON_IsObject(a) || cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(a, "order"))) continue;
        if (max < 99) max = max + 1 > 99 ? 99 : max + 1;
        cJSON_DeleteItemFromObjectCaseSensitive(a, "order");
        cJSON_AddNumberToObject(a, "order", max);
    }
    return missing;
}

void sanitize_activities_json(cJSON *root) {
    (void)sanitize_activities_json_changed(root);
}

int sanitize_activities_json_changed(cJSON *root) {
    if (!root) return 0;
    cJSON *acts = cJSON_GetObjectItemCaseSensitive(root, "activities");
    if (!acts || !cJSON_IsArray(acts)) return 0;

    /* sorted first: the ones without an order are last, and keep that place with their number */
    int changed = sort_activities_by_order(acts);
    changed |= number_unordered_activities(acts) > 0;

    cJSON *a = NULL;
    cJSON_ArrayForEach(a, acts) {
        if (!a || !cJSON_IsObject(a)) continue;

        /* A start request finds the first activity of an id: say when one cannot be reached */
        char aid[32] = "";
        cJSON *jaid = cJSON_GetObjectItemCaseSensitive(a, "id");
        if (cJSON_IsString(jaid) && jaid->valuestring) snprintf(aid, sizeof(aid), "%s", jaid->valuestring);
        else json_number_text(jaid, aid, sizeof(aid));
        if (!aid[0]) {
            fprintf(stderr, "[!] Config: activity without an id cannot be started\n");
        } else {
            cJSON *prev = NULL;
            cJSON_ArrayForEach(prev, acts) {
                if (prev == a) break;
                char pid[32] = "";
                cJSON *jp = cJSON_GetObjectItemCaseSensitive(prev, "id");
                if (cJSON_IsString(jp) && jp->valuestring) snprintf(pid, sizeof(pid), "%s", jp->valuestring);
                else json_number_text(jp, pid, sizeof(pid));
                if (strcmp(pid, aid) == 0) {
                    fprintf(stderr, "[!] Config: activity id \"%s\" is used twice: only the first is used\n", aid);
                    break;
                }
            }
        }

        cJSON *aname = cJSON_GetObjectItemCaseSensitive(a, "name");
        if (cJSON_IsString(aname) && aname->valuestring) {
            char clean[256];
            clean_display_name(aname->valuestring, clean, sizeof(clean));
            if (strcmp(clean, aname->valuestring) != 0) {
                cJSON_ReplaceItemInObjectCaseSensitive(a, "name", cJSON_CreateString(clean[0] ? clean : "Activity"));
            }
        }

        cJSON *start_seq = cJSON_GetObjectItemCaseSensitive(a, "startSequence");
        if (start_seq) sanitize_activity_steps(start_seq);

        cJSON *stop_seq = cJSON_GetObjectItemCaseSensitive(a, "stopSequence");
        if (stop_seq) sanitize_activity_steps(stop_seq);
    }
    return changed;
}
