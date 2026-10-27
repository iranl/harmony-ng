#ifndef HARMONY_RF_PAIRING_H
#define HARMONY_RF_PAIRING_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void start_pairing_mode(int slot);
void stop_pairing_mode(void);
void unpair_device(int slot);
void set_hub_led(int state);
void *hub_button_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_PAIRING_H */
