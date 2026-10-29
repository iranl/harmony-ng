/* Activity icon keyword mapping shared by the Hub push and the Logitech import.
   Build and run: cc -Ipayload/source tests/test_activity_icons.c -o /tmp/t && /tmp/t */
#include "rf_proto.h"
#include <stdio.h>

static int failures;

static void expect(const char *icon, const char *name, const char *type, const char *want) {
    const char *got = rf_activity_icon_keyword(rf_activity_icon_type(icon, name, type));
    if (strcmp(got, want) != 0) {
        printf("FAIL icon=%s name=%s type=%s: got %s, want %s\n",
               icon ? icon : "(null)", name ? name : "(null)", type ? type : "(null)", got, want);
        failures++;
    }
}

int main(void) {
    /* Every keyword, bare and as the web UI's "<keyword>.png", maps to itself */
    for (uint8_t t = 1; t <= 17; t++) {
        char png[32];
        const char *kw = rf_activity_icon_keyword(t);
        snprintf(png, sizeof(png), "%s.png", kw);
        expect(kw, "Zzz", "", kw);
        expect(png, "Zzz", "", kw);
    }

    /* Stock Logitech images saved by older web UIs */
    expect("0F40F257-68C8-4F24-9561-3CB3441DA395.png", "Watch TV", "", "apple");
    expect("72D0AAB4-2ED8-4644-84D2-0031BDAE356E_small.png", "Watch TV", "", "music");
    expect("/assets/icons/9d24eb35-468b-4148-a6a2-bb764aa9609c_smallon.png", "x", "", "switch");

    /* An icon that names nothing falls back to the activity name, then its type, then TV */
    expect("A1164C5F-1D7A-4B7F-BBEC-E9F5ACF303B7_small.png", "Listen to Music", "", "music");
    expect(NULL, "Kitchen", "VirtualTelevisionN", "watchtv");
    expect(NULL, "Kitchen", "Den", "watchtv");

    /* Brand and word-boundary cases */
    expect(NULL, "Apple TV", "", "apple");
    expect(NULL, "Apple Music", "", "music");
    expect(NULL, "Play PS5", "PlayGame", "ps");
    expect(NULL, "PS5", "", "ps");
    expect(NULL, "Smart TV Apps", "", "smarttv");
    expect(NULL, "Upstairs Apps", "", "watchtv");
    expect(NULL, "Lamp", "", "light");
    expect(NULL, "Lamps", "", "light");
    expect(NULL, "Office", "", "watchtv");
    expect(NULL, "Fireplace", "", "watchtv");
    expect(NULL, "Fire TV", "", "firetv");
    expect(NULL, "Smart Lights", "", "light");
    expect(NULL, "PowerOff", "", "power");
    expect(NULL, "Kitchen", "VirtualDvd", "movie");

    if (failures) return 1;
    printf("ALL ACTIVITY ICON MAPPING TESTS PASSED!\n");
    return 0;
}
