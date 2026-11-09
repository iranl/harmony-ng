/*
 * Model of a paired remote (rf_state.c, rf_api.c): the EquadID names the family, a Juniper
 * (touchscreen) remote's product comes from the skin id it reports in RF_MSG_UNIT_ID, Sprite
 * remotes are told apart by the EquadID, and nothing a slot sends replaces an identified model.
 *
 * Build: gcc -Wall -O2 -Ipayload/source -Itests tests/test_rf_model.c payload/source/rf_state.c \
 *            payload/source/rf_api.c payload/source/cJSON.c -lpthread -lm
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rf_state.h"
#include "rf_proto.h"
#include "rf_api.h"
#include "cJSON.h"
#include "rf_api_stubs.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)
#define CHECK_STR(got, want, msg) do { const char *g_ = (got); if (strcmp(g_, (want)) != 0) { g_failures++; \
    printf("  FAIL %s: got '%s', want '%s' (line %d)\n", msg, g_, (want), __LINE__); } } while (0)
#define CHECK_NULL(got, msg) do { const char *g_ = (got); if (g_ != NULL) { g_failures++; \
    printf("  FAIL %s: got '%s', want NULL (line %d)\n", msg, g_, __LINE__); } } while (0)

static void reset(void) {
    memset(g_paired_remotes, 0, sizeof(g_paired_remotes));
    memset(g_remote_versions, 0, sizeof(g_remote_versions));
}

/* unit-id frame payload: id, then the skin byte at RF_UNIT_ID_SKIN_OFF */
static void unit_id_payload(uint8_t p[25], const char *id, uint8_t skin) {
    memset(p, 0, 25);
    size_t n = strlen(id);
    p[0] = (uint8_t)n;
    memcpy(p + 1, id, n);
    p[RF_UNIT_ID_SKIN_OFF] = skin;
}

static cJSON *slot_json(int slot) {
    static char body[4096];
    if (rf_build_status_json(body, sizeof(body), RF_STATUS_STABLE_ONLY) <= 0) return NULL;
    cJSON *root = cJSON_Parse(body);
    if (!root) return NULL;
    cJSON *s = cJSON_DetachItemFromArray(cJSON_GetObjectItem(root, "slots"), slot - 1);
    cJSON_Delete(root);
    return s;
}

int main(void) {
    printf("remote model test\n");

    /* 1. the skin table, as the stock remote UI names it */
    struct { uint8_t skin; const char *id, *name; } table[] = {
        {0x64, "ultimate", "Harmony Ultimate"}, {0x66, "ultimate_one", "Harmony Ultimate One"},
        {0x69, "ultimate_home", "Harmony Ultimate Home"}, {0x6C, "ultimate_home", "Harmony Ultimate Home"},
        {0x6F, "elite", "Harmony Elite"}, {0x70, "950", "Harmony 950"},
        {0x63, "touch", "Harmony Touch"}, {0x01, "touch", "Harmony Touch"},
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_JUNIPER, table[i].skin), table[i].id, "skin -> model id");
        CHECK_STR(rf_model_name_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_JUNIPER, table[i].skin), table[i].name, "skin -> model name");
    }
    /* a Juniper remote without a skin id is unknown, never "elite" */
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_JUNIPER, RF_EQUAD_JUNIPER, 0), "unknown", "Juniper without skin");
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_JUNIPER, 0, 0), "unknown", "Juniper family from traffic, no skin");
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_JUNIPER, 0, 0x6F), "elite", "Juniper family from traffic with skin");
    /* Sprite remotes by EquadID; the skin is irrelevant there */
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_SMART_CONTROL, 0), "smart_control", "0x401E");
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_SPRITE_A, 0), "companion", "0x4026");
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_SPRITE_B, 0x64), "companion", "0x4027");
    CHECK_STR(rf_model_name_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_SPRITE_A, 0), "Harmony Companion", "Companion name");
    CHECK_STR(rf_model_name_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_SMART_CONTROL, 0), "Harmony Smart Control", "Smart Control name");
    /* families the Hub does not drive */
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_4024, 0x64), "unknown", "0x4024");
    CHECK_STR(rf_model_id_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_USB_DONGLE, 0), "unknown", "0x880E");
    CHECK_NULL(rf_model_name_for(REMOTE_MODEL_UNKNOWN, 0, 0), "unknown name");
    CHECK_NULL(rf_model_name_for(REMOTE_MODEL_JUNIPER, RF_EQUAD_JUNIPER, 0), "Juniper without skin: no name");
    CHECK_NULL(rf_model_name_for(REMOTE_MODEL_UNKNOWN, RF_EQUAD_4024, 0x64), "0x4024: no name");

    /* 2. a Juniper slot: unknown until the skin arrives, then the product, kept across frames */
    reset();
    g_paired_remotes[0].active = true;
    g_paired_remotes[0].equad_id = RF_EQUAD_JUNIPER;
    g_paired_remotes[0].model = REMOTE_MODEL_JUNIPER;
    snprintf(g_paired_remotes[0].name, sizeof(g_paired_remotes[0].name), "Harmony Home"); /* "Home" must not make it a Companion */
    CHECK(!is_companion_slot(1), "a Juniper slot named ...Home is not a Companion");
    cJSON *s = slot_json(1);
    CHECK(s != NULL, "status JSON");
    if (s) {
        CHECK_STR(cJSON_GetObjectItem(s, "model")->valuestring, "unknown", "Juniper slot before the skin");
        CHECK(cJSON_IsNull(cJSON_GetObjectItem(s, "skin")), "skin null before it is reported");
        CHECK(cJSON_IsNull(cJSON_GetObjectItem(s, "model_name")), "model_name null while unknown");
        CHECK_STR(cJSON_GetObjectItem(s, "name")->valuestring, "Harmony Home (Slot 1)", "slot name from the CC2544 while unknown");
        CHECK(cJSON_GetObjectItem(s, "equad_id")->valueint == 0x4021, "equad_id 16417");
        CHECK(cJSON_IsTrue(cJSON_GetObjectItem(s, "has_screen")), "a Juniper remote has a screen");
        cJSON_Delete(s);
    }
    uint8_t p[25];
    unit_id_payload(p, "TEST-UNIT-ID", 0);
    CHECK(rf_slot_apply_unit_id(1, p) == 1, "unit id stored");
    CHECK(g_remote_versions[0].skin == 0, "an older remote (skin byte 0) reports no skin");
    CHECK_STR(rf_slot_model_id(1), "unknown", "still unknown without a skin");
    unit_id_payload(p, "TEST-UNIT-ID", 0x64);
    CHECK(rf_slot_apply_unit_id(1, p) == 1, "skin is a change");
    CHECK(rf_slot_apply_unit_id(1, p) == 0, "the same frame again is no change");
    s = slot_json(1);
    if (s) {
        CHECK_STR(cJSON_GetObjectItem(s, "model")->valuestring, "ultimate", "model ultimate");
        CHECK_STR(cJSON_GetObjectItem(s, "model_name")->valuestring, "Harmony Ultimate", "model_name");
        CHECK(cJSON_GetObjectItem(s, "skin")->valueint == 100, "skin 100");
        CHECK(cJSON_GetObjectItem(s, "equad_id")->valueint == 16417, "equad_id 16417");
        CHECK_STR(cJSON_GetObjectItem(s, "name")->valuestring, "Harmony Ultimate (Slot 1)", "slot name from model_name");
        cJSON_Delete(s);
    }
    /* frames from the slot: protocol frames, a stock key packet, a unit id without skin */
    rf_slot_saw_protocol_frame(1);
    rf_note_slot_family(1, REMOTE_MODEL_COMPANION);
    unit_id_payload(p, "TEST-UNIT-ID", 0);
    rf_slot_apply_unit_id(1, p);
    CHECK_STR(rf_slot_model_id(1), "ultimate", "frames do not overwrite the model");
    CHECK(g_paired_remotes[0].model == REMOTE_MODEL_JUNIPER, "family kept");
    /* a 24-character id leaves no room for the skin byte: it is not read */
    reset();
    g_paired_remotes[0].equad_id = RF_EQUAD_JUNIPER;
    unit_id_payload(p, "", 0);
    memset(p + 1, 'A', 24);
    p[0] = 24;
    rf_slot_apply_unit_id(1, p);
    CHECK(g_remote_versions[0].skin == 0, "no skin read behind a 24-character id");

    /* 3. a Sprite slot by EquadID: a protocol frame or a skin does not turn it into a Juniper */
    reset();
    g_paired_remotes[1].active = true;
    g_paired_remotes[1].equad_id = RF_EQUAD_SPRITE_A;
    CHECK(is_companion_slot(2), "0x4026 is a Companion");
    rf_slot_saw_protocol_frame(2);
    CHECK_STR(rf_slot_model_id(2), "companion", "Companion kept after a frame");
    s = slot_json(2);
    if (s) {
        CHECK_STR(cJSON_GetObjectItem(s, "model")->valuestring, "companion", "status model companion");
        CHECK_STR(cJSON_GetObjectItem(s, "model_name")->valuestring, "Harmony Companion", "status model_name");
        CHECK(cJSON_IsNull(cJSON_GetObjectItem(s, "skin")), "no skin for a Sprite remote");
        CHECK(cJSON_GetObjectItem(s, "equad_id")->valueint == 0x4026, "equad_id 0x4026");
        CHECK(cJSON_IsFalse(cJSON_GetObjectItem(s, "has_screen")), "no screen");
        cJSON_Delete(s);
    }

    /* 4. an active slot nobody identified: unknown, the name falls back to the CC2544 name */
    reset();
    g_paired_remotes[0].active = true;
    snprintf(g_paired_remotes[0].name, sizeof(g_paired_remotes[0].name), "100");
    s = slot_json(1);
    if (s) {
        CHECK_STR(cJSON_GetObjectItem(s, "model")->valuestring, "unknown", "unidentified active slot is unknown, not elite");
        CHECK(cJSON_IsNull(cJSON_GetObjectItem(s, "equad_id")), "equad_id null when not known");
        CHECK_STR(cJSON_GetObjectItem(s, "name")->valuestring, "100 (Slot 1)", "CC2544 name while unknown");
        cJSON_Delete(s);
    }
    /* traffic names the family, the skin then names the product */
    rf_slot_saw_protocol_frame(1);
    CHECK(g_paired_remotes[0].model == REMOTE_MODEL_JUNIPER, "a protocol frame names the Juniper family");
    unit_id_payload(p, "", 0x6F);
    CHECK(rf_slot_apply_unit_id(1, p) == 1, "skin without a unit id is stored");
    CHECK_STR(rf_slot_model_id(1), "elite", "Elite from the skin");

    if (g_failures) {
        printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    printf("All remote model checks passed\n");
    return 0;
}
