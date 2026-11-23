/* adv_listen's filter and report datagram (hub_btstack/bt_listen_report.h) */
#include <assert.h>
#include "bt_listen_report.h"

int main(void) {
    uint8_t f[ADV_LISTEN_FILTER_MAX];
    assert(adv_listen_parse_filter("", f) == 0);
    assert(adv_listen_parse_filter("53050100037e05", f) == 7 && f[0] == 0x53 && f[6] == 0x05);
    assert(adv_listen_parse_filter("530", f) == -1);
    assert(adv_listen_parse_filter("53zz", f) == -1);
    assert(adv_listen_parse_filter("000102030405060708090A0B0C0D0E0F10", f) == -1); /* 17 bytes */

    /* A Switch 2 wake advertisement (alexvnesta/switch2controller wake-packet.md) */
    static const uint8_t ad[31] = {
        0x02, 0x01, 0x06, 0x1B, 0xFF, 0x53, 0x05, 0x01, 0x00, 0x03, 0x7E, 0x05, 0x66, 0x20,
        0x00, 0x01, 0x81, 0x97, 0x1F, 0x99, 0x55, 0xE2, 0x98, 0x0F, 0, 0, 0, 0, 0, 0, 0,
    };
    int n = adv_listen_parse_filter("53050100037E05", f);
    assert(adv_listen_match(ad, 31, f, n));
    assert(adv_listen_match(ad, 31, f, 0));
    assert(!adv_listen_match(ad, 10, f, n)); /* data cut before the filter ends */
    f[0] = 0x54;
    assert(!adv_listen_match(ad, 31, f, n));

    char out[384];
    static const uint8_t addr[6] = { 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03 };
    assert(adv_listen_format(out, sizeof(out), "switch2-wake", addr, 0, 3, -61, ad, 3) > 0);
    assert(strcmp(out, "{\"plugin_id\":\"switch2-wake\",\"command\":\"adv_report\",\"action\":\"tap\",\"value\":0,"
                       "\"addr\":\"AA:BB:CC:01:02:03\",\"addr_type\":0,\"event_type\":3,\"rssi\":-61,\"data\":\"020106\"}") == 0);
    assert(adv_listen_format(out, sizeof(out), "p", addr, 0, 0, 0, ad, 31) > 0 && strstr(out, "\"data\":\"0201061BFF53") != NULL);
    assert(adv_listen_format(out, 40, "p", addr, 0, 0, 0, ad, 31) == -1);
    assert(adv_listen_format(out, sizeof(out), "p", addr, 0, 0, 0, ad, 32) == -1);

    printf("[+] test_bt_listen_report: filter and report datagram\n");
    return 0;
}
