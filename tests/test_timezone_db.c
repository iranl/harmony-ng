#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "timezone_db.h"

int main(void) {
    printf("[*] Running timezone_db unit tests...\n");

    char out[64];

    /* 1. Test IANA to POSIX resolution */
    timezone_to_posix("Europe/Amsterdam", out, sizeof(out));
    assert(strcmp(out, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);

    timezone_to_posix("europe/berlin", out, sizeof(out)); /* case-insensitive */
    assert(strcmp(out, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);

    timezone_to_posix("America/New_York", out, sizeof(out));
    assert(strcmp(out, "EST5EDT,M3.2.0,M11.1.0") == 0);

    timezone_to_posix("Asia/Tokyo", out, sizeof(out));
    assert(strcmp(out, "JST-9") == 0);

    timezone_to_posix("Asia/Jerusalem", out, sizeof(out));
    assert(strcmp(out, "IST-2IDT,M3.4.4/26,M10.5.0") == 0);

    timezone_to_posix("Australia/Sydney", out, sizeof(out));
    assert(strcmp(out, "AEST-10AEDT,M10.1.0,M4.1.0/3") == 0);

    timezone_to_posix("UTC", out, sizeof(out));
    assert(strcmp(out, "UTC0") == 0);

    /* 2. Test explicit POSIX rule pass-through */
    timezone_to_posix("CUST-2CDT,M3.1.0,M10.1.0", out, sizeof(out));
    assert(strcmp(out, "CUST-2CDT,M3.1.0,M10.1.0") == 0);

    /* 3. Test RF prefix extraction (<= 14 bytes) */
    char prefix[32];
    timezone_get_rf_prefix("CET-1CEST,M3.5.0,M10.5.0/3", prefix, sizeof(prefix));
    assert(strcmp(prefix, "CET-1CEST") == 0);
    assert(strlen(prefix) <= 14);

    timezone_get_rf_prefix("EST5EDT,M3.2.0,M11.1.0", prefix, sizeof(prefix));
    assert(strcmp(prefix, "EST5EDT") == 0);
    assert(strlen(prefix) <= 14);

    timezone_get_rf_prefix("ACST-9:30ACDT,M10.1.0,M4.1.0/3", prefix, sizeof(prefix));
    assert(strcmp(prefix, "ACST-9:30ACDT") == 0);
    assert(strlen(prefix) <= 14);

    timezone_get_rf_prefix("VERYLONGCUSTOMPREFIXTHATEXCEEDS14BYTES", prefix, sizeof(prefix));
    assert(strlen(prefix) <= 14);

    /* 4. Test Remote prefix expansion */
    timezone_expand_prefix("CET-1CEST", out, sizeof(out));
    assert(strcmp(out, "CET-1CEST,M3.5.0,M10.5.0/3") == 0);

    timezone_expand_prefix("EST5EDT", out, sizeof(out));
    assert(strcmp(out, "EST5EDT,M3.2.0,M11.1.0") == 0);

    timezone_expand_prefix("IST-2IDT", out, sizeof(out));
    assert(strcmp(out, "IST-2IDT,M3.4.4/26,M10.5.0") == 0);

    timezone_expand_prefix("BRT3BRST", out, sizeof(out));
    assert(strcmp(out, "BRT3BRST,M10.3.0/0,M2.3.0/0") == 0);

    timezone_expand_prefix("JST-9", out, sizeof(out));
    assert(strcmp(out, "JST-9") == 0);

    timezone_expand_prefix("UTC", out, sizeof(out));
    assert(strcmp(out, "UTC") == 0);

    /* In-place expansion safety */
    char in_place[64] = "EST5EDT";
    timezone_expand_prefix(in_place, in_place, sizeof(in_place));
    assert(strcmp(in_place, "EST5EDT,M3.2.0,M11.1.0") == 0);

    printf("[+] All timezone_db unit tests passed!\n");
    return 0;
}
