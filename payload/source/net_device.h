#ifndef NET_DEVICE_H
#define NET_DEVICE_H

/* HTTP(S) and TCP devices: a command is a request to netHost:netPort (TLS optional).
 * Stored fields (config_sanitize.c keeps only valid ones):
 *   device:  type "HTTP" | "TCP", netHost, netPort, netTls, netTlsVerify ("verify" | "pin" |
 *            "insecure"), netTimeoutMs; HTTP also httpHeaders [{name, value}], httpAuthType
 *            ("none" | "basic" | "bearer"), httpUser, httpPassword, httpToken
 *   command: keyCode "HTTP:<name>" with httpMethod, httpPath, httpBody, httpContentType, or
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

#endif
