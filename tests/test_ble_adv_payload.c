#include "ble_adv_payload.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    uint8_t buf[BLE_ADV_MAX_LEN];
    const char *end = NULL;

    /* Test 1: flags + complete list of six 32-bit service UUIDs (29 bytes) */
    const char *mantra = "02011A19054E6F720E0438064C2E561510D42DEB8C15D3B51804030201";
    assert(ble_adv_parse_hex(mantra, buf, &end) == 29);
    assert(*end == '\0');
    assert(buf[0] == 0x02 && buf[3] == 0x19 && buf[4] == 0x05 && buf[28] == 0x01);
    printf("Test 1 valid payload: OK\n");

    /* Test 2: parsing stops at a space so a duration can follow */
    assert(ble_adv_parse_hex("0201060303AAFE 1500", buf, &end) == 7);
    assert(strcmp(end, " 1500") == 0);
    printf("Test 2 stops at space: OK\n");

    /* Test 3: lowercase hex */
    assert(ble_adv_parse_hex("020106", buf, NULL) == 3);
    assert(ble_adv_parse_hex("0201aa", buf, NULL) == 3 && buf[2] == 0xAA);
    printf("Test 3 lowercase: OK\n");

    /* Test 4: odd digit count, non-hex characters and empty input are rejected */
    assert(ble_adv_parse_hex("02010", buf, NULL) == -1);
    assert(ble_adv_parse_hex("0201G6", buf, NULL) == -1);
    assert(ble_adv_parse_hex("", buf, NULL) == -1);
    assert(ble_adv_parse_hex(NULL, buf, NULL) == -1);
    printf("Test 4 malformed hex: OK\n");

    /* Test 5: AD structures must fill the payload exactly */
    assert(ble_adv_parse_hex("030106", buf, NULL) == -1);   /* length runs past the end */
    assert(ble_adv_parse_hex("02010600", buf, NULL) == -1); /* zero-length structure */
    assert(ble_adv_parse_hex("0201", buf, NULL) == -1);     /* truncated */
    printf("Test 5 AD structure: OK\n");

    /* Test 6: more than 31 bytes is rejected */
    char long_hex[2 * 32 + 1];
    long_hex[0] = '1'; long_hex[1] = 'F'; /* one structure claiming 31 bytes */
    for (int i = 2; i < 2 * 32; i++) long_hex[i] = '0';
    long_hex[2 * 32] = '\0';
    assert(ble_adv_parse_hex(long_hex, buf, NULL) == -1);
    long_hex[1] = 'E'; long_hex[2 * 31] = '\0'; /* 31 bytes total fits */
    assert(ble_adv_parse_hex(long_hex, buf, NULL) == 31);
    printf("Test 6 length limit: OK\n");

    printf("ALL BLE ADVERTISING PAYLOAD TESTS PASSED!\n");
    return 0;
}
