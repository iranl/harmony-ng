#include "mantra_codec.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

static int hex_to_bytes(const char *hex, uint8_t *out) {
    int n = 0;
    while (hex[0] && hex[1]) {
        unsigned int b;
        sscanf(hex, "%2x", &b);
        out[n++] = (uint8_t)b;
        hex += 2;
    }
    return n;
}

int main(void) {
    uint8_t adv[MANTRA_ADV_LEN];
    uint8_t cmd7[7];
    uint8_t buf[64];
    uint16_t id = 0;
    int version = -1;
    int br = 100, ct = 50;

    /* Test 1: encode ha-ble-adv's "main light on" vector (id 0xC5F0, counter 1080). The body must
     * match its manufacturer-data packet; the UUID-list form wraps the same body. */
    uint8_t light_on[7] = {0x01, 0x05, 0, 0, 0, 0, 0};
    assert(mantra_encode_adv(0, 0xC5F0, 1080, light_on, adv) == MANTRA_ADV_LEN);
    hex_to_bytes("02011A19054E6F", buf);
    assert(memcmp(adv, buf, 7) == 0);
    hex_to_bytes("720E0438064C2E561510D42DEB8C15D3B518", buf);
    assert(memcmp(adv + 7, buf, MANTRA_BODY_LEN) == 0);
    hex_to_bytes("04030201", buf);
    assert(memcmp(adv + 7 + MANTRA_BODY_LEN, buf, 4) == 0);
    printf("Test 1 encode matches ha-ble-adv vector: OK\n");

    /* Test 2: decode ha-ble-adv's manufacturer-data test vector (main light on, id 0xC5F0) */
    int n = hex_to_bytes("02011A15FF4E6F720E0438064C2E561510D42DEB8C15D3B518", buf);
    assert(mantra_decode_adv(buf, n, &id, &version) == 1);
    assert(id == 0xC5F0 && version == 0);
    printf("Test 2 decode manufacturer data: OK\n");

    /* Test 3: decode ha-ble-adv's iOS UUID-list test vector */
    n = hex_to_bytes("19054E6F720E059C064D920ED13EF28E3B6539B0E07604030201", buf);
    assert(mantra_decode_adv(buf, n, &id, &version) == 1);
    assert(id == 0x255C && version == 0);
    printf("Test 3 decode UUID list: OK\n");

    /* Test 4: our own packets decode back to their id and version */
    assert(mantra_build_command(1, "LIGHT_ON", &br, &ct, cmd7) == 0);
    mantra_encode_adv(1, 0x1234, 0xBEEF, cmd7, adv);
    assert(mantra_decode_adv(adv, MANTRA_ADV_LEN, &id, &version) == 1);
    assert(id == 0x1234 && version == 1);
    printf("Test 4 round trip: OK\n");

    /* Test 5: unrelated or corrupted advertisements are ignored */
    n = hex_to_bytes("0201060303AAFE", buf);
    assert(mantra_decode_adv(buf, n, &id, &version) == 0);
    memcpy(buf, adv, MANTRA_ADV_LEN);
    buf[14] ^= 0x01; /* a whitened family byte */
    assert(mantra_decode_adv(buf, MANTRA_ADV_LEN, &id, &version) == 0);
    printf("Test 5 rejects non-Mantra data: OK\n");

    /* Test 6: named actions */
    assert(mantra_build_command(0, "light_off", &br, &ct, cmd7) == 0 && cmd7[0] == 0x01 && cmd7[1] == 0x06);
    assert(mantra_build_command(0, "FAN_ON", &br, &ct, cmd7) == 0 && cmd7[1] == 0x07);
    assert(mantra_build_command(0, "REVERSE", &br, &ct, cmd7) == 0 && cmd7[1] == 0x14);
    assert(mantra_build_command(0, "SPEED:20", &br, &ct, cmd7) == 0 &&
           cmd7[0] == 0x03 && cmd7[1] == 0x01 && cmd7[2] == 20);
    assert(mantra_build_command(1, "LIGHT_OFF", &br, &ct, cmd7) == 0 && cmd7[1] == 0x02);
    assert(mantra_build_command(0, "RAW:01:05:00:00:e1:e2:e3", &br, &ct, cmd7) == 0 &&
           cmd7[1] == 0x05 && cmd7[6] == 0xE3);
    printf("Test 6 named actions: OK\n");

    /* Test 7: brightness and colour temperature match ha-ble-adv's COLD and WARM vectors */
    br = 100;
    assert(mantra_build_command(0, "CT:0", &br, &ct, cmd7) == 0);
    uint8_t cold[7] = {0x02, 0x00, 255, 7, 6, 255, 255};
    assert(memcmp(cmd7, cold, 7) == 0);
    assert(mantra_build_command(0, "CT:100", &br, &ct, cmd7) == 0);
    uint8_t warm[7] = {0x02, 0xFF, 0, 7, 0, 255, 0};
    assert(memcmp(cmd7, warm, 7) == 0);
    assert(mantra_build_command(0, "BRIGHTNESS:50", &br, &ct, cmd7) == 0);
    assert(br == 50 && ct == 100 && cmd7[5] == 128 && cmd7[6] == 0);
    printf("Test 7 light values: OK\n");

    /* Test 8: invalid actions and values */
    assert(mantra_build_command(0, "SPEED:32", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(0, "BRIGHTNESS:0", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(0, "CT:101", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(1, "FAN_ON", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(1, "SPEED:5", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(0, "DANCE", &br, &ct, cmd7) == -1);
    assert(mantra_build_command(0, "RAW:01:05", &br, &ct, cmd7) == -1);
    assert(br == 50 && ct == 100); /* rejected values leave the light state alone */
    printf("Test 8 invalid input: OK\n");

    /* Test 9: fan id parsing */
    assert(mantra_parse_id("C5F0", &id) == 0 && id == 0xC5F0);
    assert(mantra_parse_id("0xc5f0", &id) == 0 && id == 0xC5F0);
    assert(mantra_parse_id("", &id) == -1);
    assert(mantra_parse_id("12345", &id) == -1);
    assert(mantra_parse_id("FBZ3", &id) == -1);
    printf("Test 9 id parsing: OK\n");

    /* Test 10: the pairing search packet matches the app's broadcast (counter 2; it carries no id) */
    assert(mantra_encode_pair_adv(0, 0, 0x0002, adv) == MANTRA_ADV_LEN);
    hex_to_bytes("02011A190569635F48000206BD595A62501525A7B1CFBE8AA004030201", buf);
    assert(memcmp(adv, buf, MANTRA_ADV_LEN) == 0);
    printf("Test 10 pairing search matches app capture: OK\n");

    /* Test 11: the bind packet carries the id twice after "00 01", then the family bytes */
    assert(mantra_encode_pair_adv(1, 0xC5F0, 0x0003, adv) == MANTRA_ADV_LEN);
    assert(adv[5] == 0x69 && adv[6] == 0x63 && adv[7] == 0x5F && adv[8] == 0x48);
    uint8_t body[MANTRA_BODY_LEN];
    memcpy(body, adv + 7, MANTRA_BODY_LEN);
    mantra_whiten16(body + 5, MANTRA_BODY_LEN - 5, 0x0003);
    uint8_t bind[MANTRA_BODY_LEN] = {0x5F, 0x48, 0x00, 0x03, 0x06, 0x05, 0x00, 0x01, 0xC5, 0xF0, 0xC5, 0xF0,
                                     0x12, 0x34, 0x56, 0x78, 0x00, 0x00};
    assert(memcmp(body, bind, MANTRA_BODY_LEN) == 0);
    assert(mantra_decode_adv(adv, MANTRA_ADV_LEN, &id, &version) == 0); /* not a command packet */
    printf("Test 11 pairing bind layout: OK\n");

    /* Test 12: the fan's answer to a search announces its id; this one is built for fan 0xC5F0 */
    uint8_t reply[MANTRA_BODY_LEN] = {0x72, 0x0E, 0xBA, 0x0E, 0x01, 0xF5, 0x0E, 0x01, 0x00, 0x01,
                                      0xC5, 0xF0, 0xC5, 0xF0, 0x00, 0x00, 0x00, 0x00};
    mantra_whiten16(reply + 5, MANTRA_BODY_LEN - 5, 0xBA0E);
    uint8_t reply_ad[2 + 2 + MANTRA_BODY_LEN] = {0x15, 0xFF, 0x4E, 0x6F};
    memcpy(reply_ad + 4, reply, MANTRA_BODY_LEN);
    id = 0;
    assert(mantra_decode_search_reply(reply_ad, sizeof(reply_ad), &id, &version) == 1);
    assert(id == 0xC5F0 && version == 0);
    assert(mantra_decode_adv(reply_ad, sizeof(reply_ad), &id, &version) == 0); /* not a command */
    assert(mantra_decode_search_reply(adv, MANTRA_ADV_LEN, &id, &version) == 0); /* a bind is not a reply */
    printf("Test 12 fan search reply: OK\n");

    /* Test 13: the fan's acknowledgement of a command (01 F6) also gives its id */
    uint8_t ack[MANTRA_BODY_LEN] = {0x72, 0x0E, 0x00, 0x05, 0x01, 0xF6, 0x12, 0x34, 0x56, 0x78,
                                    0xC5, 0xF0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01};
    mantra_whiten16(ack + 5, MANTRA_BODY_LEN - 5, 0x0005);
    memcpy(reply_ad + 4, ack, MANTRA_BODY_LEN);
    id = 0;
    assert(mantra_decode_adv(reply_ad, sizeof(reply_ad), &id, &version) == 1 && id == 0xC5F0);
    assert(mantra_decode_search_reply(reply_ad, sizeof(reply_ad), &id, &version) == 0);
    printf("Test 13 fan acknowledgement: OK\n");

    /* Test 14: an action without its value takes the value with each send */
    int lo = -1, hi = -1;
    assert(mantra_value_range("BRIGHTNESS", &lo, &hi) == 1 && lo == 1 && hi == 100);
    assert(mantra_value_range("speed", &lo, &hi) == 1 && lo == 0 && hi == 31);
    assert(mantra_value_range("CT", &lo, &hi) == 1 && lo == 0 && hi == 100);
    assert(mantra_value_range("BRIGHTNESS:50", &lo, &hi) == 0);
    assert(mantra_value_range("LIGHT_ON", &lo, &hi) == 0);
    assert(mantra_build_command(0, "BRIGHTNESS", &br, &ct, cmd7) == -1);
    printf("Test 14 value ranges: OK\n");

    printf("ALL MANTRA CODEC TESTS PASSED!\n");
    return 0;
}
