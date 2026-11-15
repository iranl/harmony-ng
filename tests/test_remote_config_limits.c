/*
 * test_remote_config_limits.c - the remote keeps every device command name the Hub can push.
 * The Hub pushes up to 255 names for each of 16 devices; the remote kept 512 in total and
 * acknowledged and dropped the rest, so the last devices showed few or no commands. Here three
 * devices of 200 commands are pushed as DEVICE_CMD_CHUNK frames, saved and loaded again.
 *
 * Run with the mock library and a fresh mock root:
 *   HARMONY_MOCK_DIR=<dir> LD_PRELOAD=libmock_hardware.so ./test_remote_config_limits
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_config.h"
#include "remote/remote_rf_sync.h"

static int count_of(int dev) {
    int n = 0;
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        if (g_rf_dev_cmds[i].dev_idx == dev) n++;
    }
    return n;
}

int main(void) {
    g_rf_fd = -1;
    g_rf_device_count = 3;
    for (int d = 0; d < 3; d++) {
        snprintf(g_rf_devices[d].id, sizeof(g_rf_devices[d].id), "%d", d + 1);
        snprintf(g_rf_devices[d].name, sizeof(g_rf_devices[d].name), "Device %d", d + 1);
    }
    g_rf_dev_cmd_count = 0;
    for (int d = 0; d < 3; d++) {
        for (int c = 0; c < 200; c++) {
            rf_frame_t f;
            memset(&f, 0, sizeof(f));
            f.msg_type = RF_MSG_DEVICE_CMD_CHUNK;
            f.payload[0] = (uint8_t)d;
            f.payload[1] = (uint8_t)c;
            f.payload[2] = 200;
            snprintf((char *)&f.payload[3], 22, "Cmd%d_%d", d, c);
            handle_hub_frame_remote(&f);
        }
    }
    if (count_of(0) != 200 || count_of(1) != 200 || count_of(2) != 200) {
        printf("[-] test_remote_config_limits: stored %d/%d/%d of 200/200/200 command names\n",
               count_of(0), count_of(1), count_of(2));
        return 1;
    }
    g_has_hub_config = true;
    save_rf_config();
    g_rf_dev_cmd_count = 0;
    g_rf_device_count = 0;
    load_rf_config();
    if (g_rf_device_count != 3 || count_of(2) != 200) {
        printf("[-] test_remote_config_limits: after save and load %d devices, %d names of the third\n",
               g_rf_device_count, count_of(2));
        return 1;
    }
    for (int i = 0; i < g_rf_dev_cmd_count; i++) {
        char want[23];
        snprintf(want, sizeof(want), "Cmd%d_%d", g_rf_dev_cmds[i].dev_idx, g_rf_dev_cmds[i].cmd_idx);
        if (strcmp(want, g_rf_dev_cmds[i].name) != 0) {
            printf("[-] test_remote_config_limits: %s stored as %s\n", want, g_rf_dev_cmds[i].name);
            return 1;
        }
    }
    printf("[+] test_remote_config_limits: 3 x 200 command names kept through the push, a save and a load\n");
    return 0;
}
