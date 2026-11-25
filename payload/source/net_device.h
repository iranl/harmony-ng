#ifndef NET_DEVICE_H
#define NET_DEVICE_H

/* HTTP(S) and TCP devices: a command is a request to netHost:netPort (TLS optional).
 * Stored fields (config_sanitize.c keeps only valid ones):
 *   device:  type "HTTP" | "TCP", netHost, netPort, netTls, netTlsVerify ("verify" | "pin" |
 *            "insecure"), netTimeoutMs; HTTP also httpHeaders [{name, value}], httpAuthType
 *            ("none" | "basic" | "bearer"), httpUser, httpPassword, httpToken
 *   command: keyCode "HTTP:<name>" with httpMethod, httpPath, httpBody, httpContentType and
 *            optional httpHeaders of its own (sent before the device's), or
 *            keyCode "TCP:<name>" with tcpPayload, tcpFormat ("text" | "hex")
 * httpPassword and httpToken never leave the Hub: GET /api/devices and /api/inventory send
 * them as "" with hasHttpPassword / hasHttpToken (config_mask_device_secrets). */

#include <strings.h>
#include "cJSON.h"

enum { NET_KIND_NONE = 0, NET_KIND_HTTP = 1, NET_KIND_TCP = 2 };

#define NET_MAX_HEADERS 8
#define NET_TIMEOUT_DEFAULT_MS 3000
#define NET_TIMEOUT_MAX_MS 15000
#define NET_TIMEOUT_MIN_MS 500
#define NET_BODY_MAX 2048
#define NET_PINS_FILE_NAME "tls_pins.json"

/* NET_KIND_* from the device's type (only the exact names, any case) */
static inline int net_device_kind(const cJSON *dev)
{
    const cJSON *jt = dev ? cJSON_GetObjectItemCaseSensitive(dev, "type") : NULL;
    const char *t = cJSON_IsString(jt) && jt->valuestring ? jt->valuestring : "";
    if (strcasecmp(t, "HTTP") == 0 || strcasecmp(t, "HTTPS") == 0) return NET_KIND_HTTP;
    if (strcasecmp(t, "TCP") == 0) return NET_KIND_TCP;
    return NET_KIND_NONE;
}

/* Combined devices: type "Combined", "transports" lists the ones in use, every command names
 * its own in "transport". IR, BT, MQTT and plugin settings are the usual flat device fields;
 * the HTTP and TCP connections sit in the objects "http" and "tcp", with the same keys as an
 * HTTP or TCP device (netHost, netPort, ...). */
enum {
    DEV_TR_IR = 1, DEV_TR_BT = 2, DEV_TR_HTTP = 4, DEV_TR_TCP = 8, DEV_TR_MQTT = 16, DEV_TR_PLUGIN = 32
};

static inline int device_is_combined(const cJSON *dev)
{
    const cJSON *jt = dev ? cJSON_GetObjectItemCaseSensitive(dev, "type") : NULL;
    return cJSON_IsString(jt) && jt->valuestring && strcasecmp(jt->valuestring, "Combined") == 0;
}

/* DEV_TR_* of a name ("ir", "bt", "http", "tcp", "mqtt", "plugin"), 0 for anything else */
static inline int device_transport_bit(const char *n)
{
    static const char *const names[] = { "ir", "bt", "http", "tcp", "mqtt", "plugin" };
    for (int i = 0; n && i < 6; i++) {
        if (strcasecmp(n, names[i]) == 0) return 1 << i;
    }
    return 0;
}

static inline const char *device_transport_name(int bit)
{
    static const char *const names[] = { "ir", "bt", "http", "tcp", "mqtt", "plugin" };
    for (int i = 0; i < 6; i++) {
        if (bit == 1 << i) return names[i];
    }
    return "";
}

/* The DEV_TR_* bits a combined device lists; *first gets the first one (the default) */
static inline int device_transports(const cJSON *dev, int *first)
{
    int mask = 0;
    if (first) *first = 0;
    const cJSON *arr = dev ? cJSON_GetObjectItemCaseSensitive(dev, "transports") : NULL;
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, arr) {
        int b = cJSON_IsString(t) ? device_transport_bit(t->valuestring) : 0;
        if (b && first && !*first) *first = b;
        mask |= b;
    }
    return mask;
}

/* The DEV_TR_* bit a command of a combined device is sent with: its own "transport" when the
 * device lists it, else the device's first transport (0: the device lists none) */
static inline int device_command_transport(const cJSON *dev, const cJSON *cmd)
{
    int first = 0;
    int mask = device_transports(dev, &first);
    const cJSON *jt = cmd ? cJSON_GetObjectItemCaseSensitive(cmd, "transport") : NULL;
    int b = cJSON_IsString(jt) ? device_transport_bit(jt->valuestring) : 0;
    return (b & mask) ? b : first;
}

/* The object holding the connection fields for an HTTP or TCP send: an HTTP or TCP device
 * itself, or the "http" / "tcp" object of a combined device. want: NET_KIND_* for a combined
 * device (0: the command's transport decides). *kind gets the NET_KIND_*; NULL: none. */
static inline const cJSON *net_device_conn(const cJSON *dev, const cJSON *cmd, int want, int *kind)
{
    int k = net_device_kind(dev);
    *kind = k;
    if (k) return dev;
    if (!device_is_combined(dev)) return NULL;
    if (!want) {
        int tr = device_command_transport(dev, cmd);
        want = tr == DEV_TR_HTTP ? NET_KIND_HTTP : tr == DEV_TR_TCP ? NET_KIND_TCP : 0;
    }
    int mask = device_transports(dev, NULL);
    if (!((want == NET_KIND_HTTP && (mask & DEV_TR_HTTP)) || (want == NET_KIND_TCP && (mask & DEV_TR_TCP))))
        return NULL;
    const cJSON *conn = cJSON_GetObjectItemCaseSensitive(dev, want == NET_KIND_HTTP ? "http" : "tcp");
    if (!cJSON_IsObject(conn)) return NULL;
    *kind = want;
    return conn;
}

#endif
