#ifndef RF_CONFIG_PUSH_H
#define RF_CONFIG_PUSH_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void *config_push_worker(void *arg);
/* A push runs, or the one that ran is about to run again (POST /api/rf/restart waits) */
bool rf_config_push_active(void);
/* A push skipped while a remote update was prepared or sent is started once the update ended
 * without installing (an installed update makes the remote ask for a push itself). Called once a second
 * from the main loop; 1 when it started the push. */
int rf_config_push_retry_skipped(void);

#ifdef __cplusplus
}
#endif

#endif /* RF_CONFIG_PUSH_H */
