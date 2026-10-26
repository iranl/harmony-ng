#include "sha256.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    char h[65];

    /* Test 1: Empty string */
    sha256_hex((const uint8_t*)"", 0, h);
    printf("Test 1 sha256(\"\"): %s\n", h);
    assert(strcmp(h, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0);

    /* Test 2: "abc" */
    sha256_hex((const uint8_t*)"abc", 3, h);
    printf("Test 2 sha256(\"abc\"): %s\n", h);
    assert(strcmp(h, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0);

    /* Test 3: Standard HMAC-SHA256 test vector */
    sha256_hmac_hex((const uint8_t*)"key", 3, (const uint8_t*)"The quick brown fox jumps over the lazy dog", 43, h);
    printf("Test 3 hmac_sha256: %s\n", h);
    assert(strcmp(h, "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8") == 0);

    printf("ALL SHA-256 AND HMAC-SHA256 NIST TESTS PASSED!\n");
    return 0;
}
