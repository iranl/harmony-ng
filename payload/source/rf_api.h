#ifndef RF_API_H
#define RF_API_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void *api_server_worker(void *arg);

#define RF_STATUS_QUERY_RADIO  0x1
#define RF_STATUS_STABLE_ONLY  0x2
/* Writes the GET /api/rf/status object to body; returns its length or -1 */
int rf_build_status_json(char *body, size_t len, unsigned flags);

#ifdef __cplusplus
}
#endif

#endif /* RF_API_H */
