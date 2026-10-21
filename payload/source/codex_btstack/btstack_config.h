/*
 * btstack_config.h - BTstack configuration for Harmony Hub (Codex)
 * MIPS32 big-endian, uClibc, Linux kernel HCI socket transport
 */

#ifndef BTSTACK_CONFIG_H
#define BTSTACK_CONFIG_H

#define HAVE_MALLOC
#define HAVE_POSIX_FILE_IO
#define HAVE_POSIX_TIME

// Enable Bluetooth Low Energy (BLE)
#define ENABLE_BLE
#define ENABLE_LE_CENTRAL
#define ENABLE_LE_PERIPHERAL
#define ENABLE_SOFTWARE_AES128

// Enable Classic Bluetooth (BR/EDR)
#define ENABLE_CLASSIC
#define ENABLE_SDP_SERVER
#define ENABLE_SDP_CLIENT
#define ENABLE_EXPLICIT_DEDICATED_BONDING_DISCONNECT

// Logging
#define ENABLE_LOG_ERROR
#define ENABLE_LOG_INFO
#define ENABLE_PRINTF_HEXDUMP

// Memory buffers
#define HCI_ACL_PAYLOAD_SIZE (1021 + 4)
#define HCI_INCOMING_PRE_BUFFER_SIZE 14
#define HCI_OUTGOING_PRE_BUFFER_SIZE 1

#define NVM_NUM_DEVICE_DB_ENTRIES 16
#define NVM_NUM_LINK_KEYS 16

#endif /* BTSTACK_CONFIG_H */
