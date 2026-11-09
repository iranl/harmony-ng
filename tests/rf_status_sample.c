/*
 * Prints hub_rf's RF status object (rf_build_status_json, the real builder) for a sample Hub:
 * a Harmony Ultimate in slot 1 (EquadID 0x4021, skin 0x64, reported in its unit-id frame) and an
 * empty slot 2. tests/record_fixtures.py records the HA fixtures from this output, so they carry
 * what the firmware produces. Addresses and ids are dummies; skin, EquadID and product names
 * are product-level values.
 *
 * Build: gcc -Wall -O2 -Ipayload/source tests/rf_status_sample.c payload/source/rf_state.c \
 *            payload/source/rf_api.c payload/source/cJSON.c -lpthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "rf_state.h"
#include "rf_proto.h"
#include "rf_api.h"
#include "rf_api_stubs.h"

int main(int argc, char **argv) {
    const char *version = argc > 1 ? argv[1] : "2.3.0";
    /* hub_rf's log lines go to stderr; stdout carries only the JSON */
    FILE *json_out = fdopen(dup(1), "w");
    dup2(2, 1);
    if (!json_out) return 1;
    /* the Hub's CC2544 and its own address */
    g_fw_version = 39;
    uint8_t hub[4] = {0xA1, 0xB2, 0xC3, 0xD4};
    memcpy(g_hub_rf_addr, hub, 4);
    g_has_paired_device = true;
    g_config_version = 7;
    /* slot 1: what the CC2544 reports (pipe 0x20 EquadID, 0x30 address), then the remote's frames */
    paired_remote_t *r = &g_paired_remotes[0];
    r->slot = 1;
    r->active = true;
    uint8_t addr[4] = {0xE5, 0xF6, 0x07, 0x18};
    memcpy(r->rf_addr, addr, 4);
    r->equad_id = RF_EQUAD_JUNIPER;
    r->model = REMOTE_MODEL_JUNIPER;
    r->last_seen_ms = get_monotonic_ms() - 2000;
    rf_slot_saw_protocol_frame(1);
    uint8_t p[25] = {0};
    const char *unit = "20141001120000-000002";
    p[0] = (uint8_t)strlen(unit);
    memcpy(p + 1, unit, strlen(unit));
    p[RF_UNIT_ID_SKIN_OFF] = RF_SKIN_ULTIMATE;
    rf_slot_apply_unit_id(1, p);
    snprintf(g_remote_versions[0].sw_version, sizeof(g_remote_versions[0].sw_version), "%s", version);
    g_remote_versions[0].config_version = 7;
    g_remote_batteries[0].percent = 87;
    g_remote_batteries[0].voltage_mv = 3980;
    g_remote_batteries[0].last_update_ms = 1;
    g_remote_batteries[0].reported_at = 1794000000; /* a fixed time: the fixtures do not change */
    g_remote_link[0].frames_rx = 120;
    g_remote_link[0].acked_sent = 12;
    g_remote_link[0].acked_ok = 12;
    /* slot 2: empty */
    g_paired_remotes[1].slot = 2;
    snprintf(g_remote_versions[1].sw_version, sizeof(g_remote_versions[1].sw_version), "unknown");

    char body[4096];
    if (rf_build_status_json(body, sizeof(body), 0) <= 0) return 1;
    fputs(body, json_out);
    fputc('\n', json_out);
    fclose(json_out);
    return 0;
}
