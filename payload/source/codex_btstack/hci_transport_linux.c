/*
 * hci_transport_linux.c - Standalone BTstack Linux HCI Socket Transport
 *
 * Directly interfaces with Linux kernel Bluetooth subsystem via AF_BLUETOOTH / BTPROTO_HCI.
 * Self-contained without requiring libbluetooth-dev or BlueZ headers.
 */

#define BTSTACK_FILE__ "hci_transport_linux.c"

#include "hci_transport_linux.h"

#include "btstack_bool.h"
#include "btstack_config.h"
#include "btstack_debug.h"
#include "btstack_run_loop.h"
#include "hci_transport.h"
#include "hci.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>

#ifndef AF_BLUETOOTH
#define AF_BLUETOOTH 31
#endif

#ifndef BTPROTO_HCI
#define BTPROTO_HCI 1
#endif

#ifndef HCI_CHANNEL_RAW
#define HCI_CHANNEL_RAW 0
#endif

#ifndef SOL_HCI
#define SOL_HCI 0
#endif

#ifndef HCI_FILTER
#define HCI_FILTER 2
#endif

#ifndef HCIDEVUP
#define HCIDEVUP _IOW('H', 201, int)
#endif

#ifndef HCISETRAW
#define HCISETRAW _IOW('H', 220, int)
#endif

#define HCI_COMMAND_PKT 0x01
#define HCI_ACLDATA_PKT 0x02
#define HCI_SCODATA_PKT 0x03
#define HCI_EVENT_PKT   0x04

struct sockaddr_hci {
    unsigned short hci_family;
    unsigned short hci_dev;
    unsigned short hci_channel;
};

struct hci_filter {
    uint32_t type_mask;
    uint32_t event_mask[2];
    uint16_t opcode;
};

#if !defined(HCI_OUTGOING_PRE_BUFFER_SIZE) || (HCI_OUTGOING_PRE_BUFFER_SIZE == 0)
#error HCI_OUTGOING_PRE_BUFFER_SIZE not defined.
#endif

static void (*hci_transport_linux_packet_handler)(uint8_t packet_type, uint8_t *packet, uint16_t size) = NULL;

static btstack_data_source_t hci_transport_linux_data_source;
static uint8_t hci_packet_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE + HCI_INCOMING_PACKET_BUFFER_SIZE + 1];
static uint8_t * hci_packet = &hci_packet_with_pre_buffer[HCI_INCOMING_PRE_BUFFER_SIZE];
static int hci_socket = -1;

static const hci_transport_config_linux_t * hci_transport_config_linux;

extern void log_msg(const char *msg);

static void hci_transport_linux_process_read(btstack_data_source_t * ds) {
    UNUSED(ds);
    int bytes_read = read(hci_socket, hci_packet, HCI_INCOMING_PACKET_BUFFER_SIZE);
    if (bytes_read < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK) {
            fprintf(stderr, "[hci_transport_linux] Read error: %s (errno=%d)\n", strerror(errno), errno);
        }
        return;
    }
    if (bytes_read > 1) {
        if (is_debug_log_enabled()) {
            uint8_t ptype = hci_packet[0];
            if (ptype == HCI_EVENT_PKT) {
                uint8_t evt = hci_packet[1];
                // Filter LE advertising report (0x3e subevent 0x02) to avoid log spam
                if (evt != 0x3e || (bytes_read > 3 && hci_packet[3] != 0x02)) {
                    char hex[64] = "";
                    for (int i = 0; i < bytes_read && i < 16; i++) {
                        sprintf(hex + strlen(hex), "%02x ", hci_packet[i]);
                    }
                    char m[128];
                    snprintf(m, sizeof(m), "HCI RX EVT: 0x%02x [%s]", evt, hex);
                    codex_log_debug(m);
                }
            } else if (ptype == HCI_ACLDATA_PKT) {
                uint16_t h = (hci_packet[1] | (hci_packet[2] << 8)) & 0x0fff;
                uint16_t cid = bytes_read > 8 ? (hci_packet[7] | (hci_packet[8] << 8)) : 0;
                char hex[64] = "";
                for (int i = 0; i < bytes_read && i < 20; i++) {
                    sprintf(hex + strlen(hex), "%02x ", hci_packet[i]);
                }
                char m[140];
                snprintf(m, sizeof(m), "HCI RX ACL: h=0x%04x cid=0x%04x [%s]", h, cid, hex);
                codex_log_debug(m);
            }
        }
        if (hci_transport_linux_packet_handler) {
            hci_transport_linux_packet_handler(hci_packet[0], &hci_packet[1], (uint16_t)(bytes_read - 1));
        }
    }
}

static void hci_transport_linux_process(btstack_data_source_t *ds, btstack_data_source_callback_type_t callback_type) {
    switch (callback_type) {
        case DATA_SOURCE_CALLBACK_READ:
            hci_transport_linux_process_read(ds);
            break;
        default:
            break;
    }
}

static void hci_transport_linux_init(const void *transport_config) {
    btstack_assert(transport_config != NULL);
    hci_transport_config_linux = (const hci_transport_config_linux_t *) transport_config;
    btstack_assert(hci_transport_config_linux->type == HCI_TRANSPORT_CONFIG_LINUX);
    log_info("Using device id %u", (unsigned int)hci_transport_config_linux->device_id);
}

static int hci_transport_linux_open(void) {
    struct sockaddr_hci addr;
    struct hci_filter flt;

    for (int retry = 0; retry < 15; retry++) {
        hci_socket = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_HCI);
        if (hci_socket < 0) {
            fprintf(stderr, "[hci_transport_linux] Failed to create HCI socket: %s\n", strerror(errno));
            sleep(1);
            continue;
        }

        fcntl(hci_socket, F_SETFD, FD_CLOEXEC);
        fcntl(hci_socket, F_SETFL, fcntl(hci_socket, F_GETFL, 0) | O_NONBLOCK);

        // Bring up adapter if needed
        ioctl(hci_socket, HCIDEVUP, hci_transport_config_linux->device_id);

        memset(&addr, 0, sizeof(addr));
        addr.hci_family = AF_BLUETOOTH;
        addr.hci_dev = (unsigned short)hci_transport_config_linux->device_id;
        addr.hci_channel = HCI_CHANNEL_RAW;

        if (bind(hci_socket, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            break;
        }

        if (retry < 14) {
            fprintf(stderr, "[hci_transport_linux] Waiting for hci%u to appear (%d/15): %s\n",
                    addr.hci_dev, retry + 1, strerror(errno));
            close(hci_socket);
            hci_socket = -1;
            sleep(1);
        }
    }

    if (hci_socket < 0) {
        fprintf(stderr, "[hci_transport_linux] Failed to bind HCI socket to hci%u: %s\n",
                (unsigned short)hci_transport_config_linux->device_id, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[hci_transport_linux] Bound socket to hci%u (RAW channel)\n", addr.hci_dev);

    // Disable kernel L2CAP/ACL processing so kernel doesn't interfere or disconnect links
    if (ioctl(hci_socket, HCISETRAW, 1) < 0) {
        fprintf(stderr, "[hci_transport_linux] Note: ioctl HCISETRAW returned: %s (errno=%d)\n", strerror(errno), errno);
    } else {
        fprintf(stderr, "[hci_transport_linux] HCISETRAW enabled: kernel Bluetooth processing disabled\n");
        log_msg("[hci_transport_linux] HCISETRAW enabled: kernel Bluetooth processing disabled");
    }

    // Configure filter so kernel forwards incoming events and ACL data
    memset(&flt, 0, sizeof(flt));
    flt.type_mask = (1 << HCI_COMMAND_PKT) | (1 << HCI_ACLDATA_PKT) | (1 << HCI_EVENT_PKT);
    flt.event_mask[0] = 0xffffffff;
    flt.event_mask[1] = 0xffffffff;
    flt.opcode = 0;

    if (setsockopt(hci_socket, SOL_HCI, HCI_FILTER, &flt, sizeof(flt)) < 0) {
        fprintf(stderr, "[hci_transport_linux] Failed to set HCI_FILTER: %s\n", strerror(errno));
    } else {
        fprintf(stderr, "[hci_transport_linux] HCI_FILTER enabled successfully\n");
    }

    btstack_run_loop_set_data_source_fd(&hci_transport_linux_data_source, hci_socket);
    btstack_run_loop_set_data_source_handler(&hci_transport_linux_data_source, &hci_transport_linux_process);
    btstack_run_loop_add_data_source(&hci_transport_linux_data_source);
    btstack_run_loop_enable_data_source_callbacks(&hci_transport_linux_data_source, DATA_SOURCE_CALLBACK_READ);

    fprintf(stderr, "[hci_transport_linux] HCI transport ready on fd %d\n", hci_socket);
    return 0;
}

static int hci_transport_linux_close(void) {
    btstack_run_loop_remove_data_source(&hci_transport_linux_data_source);
    if (hci_socket >= 0) {
        close(hci_socket);
        hci_socket = -1;
    }
    return 0;
}

static void hci_transport_linux_register_packet_handler(void (*handler)(uint8_t packet_type, uint8_t *packet, uint16_t size)) {
    hci_transport_linux_packet_handler = handler;
}

static int hci_transport_linux_send_packet(uint8_t packet_type, uint8_t * packet, int size) {
    uint8_t * buffer = &packet[-1];
    uint32_t  buffer_size = (uint32_t)size + 1;
    buffer[0] = packet_type;

    if (is_debug_log_enabled()) {
        if (packet_type == HCI_COMMAND_PKT && size >= 2) {
            uint16_t op = packet[0] | (packet[1] << 8);
            char m[128];
            snprintf(m, sizeof(m), "HCI TX CMD: opcode=0x%04x len=%d (p0=0x%02x, p1=0x%02x, p2=0x%02x)",
                     op, size, packet[0], packet[1], size > 2 ? packet[2] : 0);
            codex_log_debug(m);
        } else if (packet_type == HCI_ACLDATA_PKT) {
            uint16_t h = (packet[0] | (packet[1] << 8)) & 0x0fff;
            char hex[64] = "";
            for (int i = 0; i < size && i < 20; i++) {
                sprintf(hex + strlen(hex), "%02x ", packet[i]);
            }
            char m[140];
            snprintf(m, sizeof(m), "HCI TX ACL: h=0x%04x [%s]", h, hex);
            codex_log_debug(m);
        }
    }

    int written = write(hci_socket, buffer, buffer_size);
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(hci_socket, &wfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
        if (select(hci_socket + 1, NULL, &wfds, NULL, &tv) > 0) {
            written = write(hci_socket, buffer, buffer_size);
        }
    }
    if (written < 0) {
        fprintf(stderr, "[hci_transport_linux] Write failed (type=%u, len=%d): %s\n",
                packet_type, size, strerror(errno));
        return -1;
    }
    return 0;
}

static const hci_transport_t hci_transport_linux = {
    .name = "Linux Raw HCI",
    .init = &hci_transport_linux_init,
    .open = &hci_transport_linux_open,
    .close = &hci_transport_linux_close,
    .register_packet_handler = &hci_transport_linux_register_packet_handler,
    .send_packet = &hci_transport_linux_send_packet
};

const hci_transport_t * hci_transport_linux_instance(void) {
    return &hci_transport_linux;
}
