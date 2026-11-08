#ifndef HUB_MDNS_H
#define HUB_MDNS_H

/*
 * mDNS / DNS-SD responder for _harmony-ng._tcp.local (spec 4.1). Runs on the main select()
 * loop: one UDP socket, no thread. The packet builder and parser are pure functions so that
 * tests/test_mdns_packet.c can check the bytes.
 */

#include <stddef.h>
#include <stdint.h>
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MDNS_PORT            5353
#define MDNS_GROUP           "224.0.0.251"
#define MDNS_SERVICE         "_harmony-ng._tcp.local"
#define MDNS_ENUM_SERVICE    "_services._dns-sd._udp.local"
#define MDNS_TTL_SHORT       120    /* A, SRV */
#define MDNS_TTL_LONG        4500   /* PTR, TXT */
#define MDNS_MAX_PACKET      1472
#define MDNS_TXT_MAX         400

/* What the responder advertises; built from the settings and the hub identity */
typedef struct {
    char instance[96];     /* "<name>._harmony-ng._tcp.local" (label-safe name) */
    char hostname[80];     /* "harmony-ng-<hub_id>.local" */
    uint16_t port;         /* 8089 */
    uint8_t ip[4];         /* A record */
    char txt[MDNS_TXT_MAX];/* TXT rdata: length-prefixed strings, in TXT key order */
    size_t txt_len;
} mdns_info_t;

/* Records to put in a response (bit mask) */
#define MDNS_R_PTR    0x01  /* service PTR -> instance */
#define MDNS_R_SRV    0x02
#define MDNS_R_TXT    0x04
#define MDNS_R_A      0x08
#define MDNS_R_ENUM   0x10  /* _services._dns-sd._udp PTR -> service type */
#define MDNS_R_ALL    (MDNS_R_PTR | MDNS_R_SRV | MDNS_R_TXT | MDNS_R_A)

/* Parsed question or answer of a query */
typedef struct {
    char name[256];
    uint16_t type;
    uint16_t cls;      /* with the QU / cache-flush bit cleared */
    int unicast;       /* QU bit (questions) */
    uint32_t ttl;      /* answers only */
    char rdname[256];  /* PTR rdata (answers only), empty otherwise */
} mdns_rr_t;

typedef struct {
    uint16_t id;
    uint16_t flags;
    int nq;
    int nan;
    mdns_rr_t q[8];
    mdns_rr_t an[8];
} mdns_query_t;

/* Fills info from the settings, the hub identity and the active interface's IP; name_suffix (may
 * be NULL) is appended to the instance name after a conflict. */
void mdns_make_info(mdns_info_t *info, const struct hub_settings *settings, const char *name_suffix);

/* Builds the TXT rdata: id, ver, api, auth, mqtt, topic (when mqtt=1), legacy, name */
size_t mdns_build_txt(char *out, size_t cap, const char *hub_id, const char *version, int api_version,
                      int auth, int mqtt, const char *topic, int legacy, const char *name);

/* Response packet (flags 0x8400) with the requested records; the records answered in the
 * "answers" set go in the answer section, the rest in additionals. ttl_zero = goodbye.
 * Returns the length or -1. */
int mdns_build_response(const mdns_info_t *info, unsigned answers, unsigned additionals,
                        int ttl_zero, uint16_t id, uint8_t *out, size_t cap);

/* Probe query (RFC 6762 8.1): QU questions for the instance and the hostname, type ANY, with
 * our SRV and A in the authority section. */
int mdns_build_probe(const mdns_info_t *info, uint8_t *out, size_t cap);

/* Parses the header, questions and answers of a packet (answers: name, type, class, TTL and
 * PTR rdata only). Returns 0 or -1. */
int mdns_parse_query(const uint8_t *pkt, size_t len, mdns_query_t *q);

/* Case-insensitive DNS name compare */
int mdns_name_eq(const char *a, const char *b);

/* Responder lifecycle (main loop). port: MDNS_PORT on the Hub. In the test build (TEST_HARNESS)
 * the responder runs only on another port (--mdns-port) and only over loopback, TTL 0. */
int hub_mdns_init(int port);
int hub_mdns_fd(void);               /* -1 when not running */
void hub_mdns_handle(void);          /* a packet is readable */
void hub_mdns_tick(void);            /* once per loop iteration */
void hub_mdns_apply_settings(const struct hub_settings *s); /* mdns_enabled, TXT values */
void hub_mdns_notify_change(void);   /* a TXT value may have changed (auth, MQTT) */
void hub_mdns_shutdown(void);        /* goodbye + close */
const char *hub_mdns_instance(void); /* current instance name, for tests and logs */

#ifdef __cplusplus
}
#endif

#endif /* HUB_MDNS_H */
