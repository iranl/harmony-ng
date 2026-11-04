#ifndef RF_HELP_H
#define RF_HELP_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Help for a remote: RF_MSG_HELP_REQUEST and RF_MSG_HELP_ANSWER frames */
void rf_help_request(const rf_frame_t *f);
void rf_help_answer(const rf_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* RF_HELP_H */
