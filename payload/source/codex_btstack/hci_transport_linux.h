/*
 * hci_transport_linux.h - BTstack Linux HCI transport interface
 */

#ifndef HCI_TRANSPORT_LINUX_H
#define HCI_TRANSPORT_LINUX_H

#include "hci_transport.h"

#if defined __cplusplus
extern "C" {
#endif

const hci_transport_t * hci_transport_linux_instance(void);
void log_msg(const char *msg);
void codex_log_debug(const char *msg);
bool is_debug_log_enabled(void);

#if defined __cplusplus
}
#endif

#endif /* HCI_TRANSPORT_LINUX_H */
