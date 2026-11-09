#ifndef HUB_SSDP_H
#define HUB_SSDP_H

/*
 * SSDP responder for the Logitech protocol shim (spec 6.6): answers M-SEARCH for ssdp:all,
 * upnp:rootdevice and urn:myharmony-com:device:harmony:1 with the location of /description on
 * the shim port, sends NOTIFY alive on enable and every 900 s, byebye on disable and shutdown.
 * Main loop, one UDP socket, active only while legacy_enabled.
 */

#include <stddef.h>
#include "config_store.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSDP_PORT   1900
#define SSDP_GROUP  "239.255.255.250"
#define SSDP_URN    "urn:myharmony-com:device:harmony:1"

/* port: SSDP_PORT on the Hub, a test port, or 0 = responder off; in the test build (TEST_HARNESS)
 * the responder runs only on a port other than SSDP_PORT and only over loopback, TTL 0.
 * shim_port: where /description is served */
int hub_ssdp_init(int port, int shim_port);
int hub_ssdp_fd(void);
void hub_ssdp_handle(void);
void hub_ssdp_tick(void);
void hub_ssdp_apply_settings(const struct hub_settings *s);
void hub_ssdp_shutdown(void);

/* uuid:<v5 from hub_id>, 36 characters without the prefix */
const char *hub_ssdp_uuid(void);
/* The UPnP device description XML for GET /description (new string, caller frees) */
char *hub_ssdp_description_xml(void);
/* M-SEARCH reply for a given ST (pure, for the tests); returns the length */
int hub_ssdp_build_response(const char *st, const char *ip, int shim_port, char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* HUB_SSDP_H */
