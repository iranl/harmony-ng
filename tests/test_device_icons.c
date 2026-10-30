#include "rf_proto.h"
#include "elite_stock_icons.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    /* Test packing and unpacking RF_MSG_DEVICE_LIST_CHUNK with 16-bit icon_id */
    uint8_t payload[26] = {0};
    uint8_t dev_idx = 2;
    uint8_t dev_count = 5;
    uint8_t icon_type = rf_activity_icon_type("shield.png", "Shield TV", "MediaStreamer");
    assert(icon_type == 8); // shield keyword = 8

    uint16_t stock_id = lookup_stock_icon_id("shield");
    assert(stock_id > 0);
    assert(stock_id < 0x8000);
    const char *stock_path = get_stock_icon_path(stock_id);
    assert(stock_path != NULL);
    assert(strstr(stock_path, "shield.png") != NULL);

    payload[0] = dev_idx;
    payload[1] = dev_count;
    payload[2] = icon_type; // 8 < 0x20
    memcpy(&payload[3], "7001", 4);
    strncpy((char *)&payload[7], "Shield TV Pro", 16);
    payload[23] = (uint8_t)(stock_id & 0xFF);
    payload[24] = (uint8_t)((stock_id >> 8) & 0xFF);

    /* Unpack on remote */
    assert(payload[2] < 0x20);
    uint8_t unpacked_itype = payload[2];
    assert(unpacked_itype == 8);

    char dev_id[8] = {0};
    char dev_name[24] = {0};
    memcpy(dev_id, &payload[3], 4);
    char tmp_name[17] = {0};
    memcpy(tmp_name, &payload[7], 16);
    strncpy(dev_name, tmp_name, sizeof(dev_name) - 1);
    assert(strcmp(dev_id, "7001") == 0);
    assert(strcmp(dev_name, "Shield TV Pro") == 0);

    uint16_t unpacked_icon_id = (uint16_t)payload[23] | ((uint16_t)payload[24] << 8);
    assert(unpacked_icon_id == stock_id);

    const char *unpacked_stock_path = get_stock_icon_path(unpacked_icon_id);
    assert(strcmp(unpacked_stock_path, stock_path) == 0);

    /* Custom icon with high bit set (e.g. 0x8001 / 32769) */
    uint16_t custom_id = 0x8001;
    payload[23] = (uint8_t)(custom_id & 0xFF);
    payload[24] = (uint8_t)((custom_id >> 8) & 0xFF);
    uint16_t unpacked_custom_id = (uint16_t)payload[23] | ((uint16_t)payload[24] << 8);
    assert(unpacked_custom_id == 0x8001);
    assert(unpacked_custom_id & 0x8000);

    char custom_expected_path[128];
    snprintf(custom_expected_path, sizeof(custom_expected_path), "/data/assets/custom/%u.png", (unsigned int)unpacked_custom_id);
    assert(strcmp(custom_expected_path, "/data/assets/custom/32769.png") == 0);

    /* Legacy compatibility: payload[2] is ASCII ID >= 0x20 */
    uint8_t legacy_payload[26] = {0};
    legacy_payload[0] = 0;
    legacy_payload[1] = 1;
    memcpy(&legacy_payload[2], "1234", 4);
    memcpy(&legacy_payload[6], "Living Room TV", 14);

    assert(legacy_payload[2] >= 0x20); // '1' is 0x31 >= 0x20
    char leg_id[8] = {0};
    char leg_name[24] = {0};
    memcpy(leg_id, &legacy_payload[2], 4);
    memcpy(leg_name, &legacy_payload[6], 20);
    assert(strcmp(leg_id, "1234") == 0);
    assert(strcmp(leg_name, "Living Room TV") == 0);

    printf("DEVICE RF CHUNK PACKING & UNPACKING TESTS PASSED!\n");
    return 0;
}
