#ifndef HARMONY_RF_PAIRING_H
#define HARMONY_RF_PAIRING_H

#include "rf_state.h"

#ifdef __cplusplus
extern "C" {
#endif

void start_pairing_mode(int slot);
/* A cancel: the lock closes and the chosen slot is dropped */
void stop_pairing_mode(void);
/* The radio reported a pairing: the lock closes, the chosen slot stays for the pipe-table reply */
void stop_pairing_after_success(void);
void unpair_device(int slot);
/* The Hub light: a state for an event, back to the led_mode setting, or slow red
   for 5 s and then back to led_mode, unless the light changed again by then */
void hub_led_show(int value);
void hub_led_restore(void);
void hub_led_show_failure(void);
void *hub_button_worker(void *arg);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_RF_PAIRING_H */
