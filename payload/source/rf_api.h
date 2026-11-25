#ifndef RF_API_H
#define RF_API_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void *api_server_worker(void *arg);

/* The first n bytes of a request in a buffer of cap bytes: 0 when its body (Content-Length) is
 * complete or it has none, 413 when the body does not fit the buffer, 400 when it stopped early */
int rf_api_body_status(const char *buf, size_t n, size_t cap);
/* POST /api/rf/layer?slot=N&on=0|1[&name=]: sets the slot's key layer (rf_keymap_layer_set()).
 * 0 when set; 400 for no slot or one outside 1 to MAX_PAIRED_REMOTES, no "on" or one other
 * than 0 or 1, or a name rf_keymap_layer_set() refuses. buf is the whole request. */
int rf_api_layer_post(const char *buf);

#define RF_STATUS_QUERY_RADIO  0x1
#define RF_STATUS_STABLE_ONLY  0x2
/* Writes the GET /api/rf/status object to body; returns its length or -1 */
int rf_build_status_json(char *body, size_t len, unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* RF_API_H */
