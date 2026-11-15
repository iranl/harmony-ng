/*
 * is_safe_ipv4() (http_util.c) on long runs of digits: it multiplied before it looked at the
 * digit count, so ten or more digits overflowed a signed int (undefined behaviour). Built with
 * -fsanitize=signed-integer-overflow -fno-sanitize-recover=all, so an overflow ends the test.
 *
 * Build: gcc -Wall -O2 -fsanitize=signed-integer-overflow -fno-sanitize-recover=all \
 *            -Ipayload/source tests/test_ipv4_digits.c payload/source/http_util.c payload/source/cJSON.c
 */
#include <stdio.h>
#include "http_util.h"

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s\n", msg); } else printf("  PASS %s\n", msg); } while (0)

int main(void) {
    printf("[-] is_safe_ipv4 with long digit runs\n");
    CHECK(is_safe_ipv4("1111111111111.1.1.1") == 0, "13 digits refused, no overflow");
    CHECK(is_safe_ipv4("1.1.1.99999999999999999999") == 0, "20 digits at the end refused");
    CHECK(is_safe_ipv4("0001.1.1.1") == 0, "4 digits refused");
    CHECK(is_safe_ipv4("192.168.1.20") == 1, "plain address accepted");
    CHECK(is_safe_ipv4("255.255.255.0") == 1, "netmask accepted");
    CHECK(is_safe_ipv4("256.1.1.1") == 0, "octet over 255 refused");
    if (g_failures) return 1;
    printf("[+] is_safe_ipv4: all checks passed\n");
    return 0;
}
