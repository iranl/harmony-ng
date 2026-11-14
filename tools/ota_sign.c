/* OTA manifest signing tool (host side, release workflow).
 *
 *   ota_sign keygen              new Ed25519 key pair: prints the secret (for the repository
 *                                secret OTA_SIGNING_KEY) and the public key line for
 *                                payload/source/ota_keys.h
 *   ota_sign sign <file>         prints the signature of <file> as 128 hex characters; the
 *                                secret (64 hex characters) comes from $OTA_SIGNING_KEY
 *   ota_sign pubkey              prints the public key of $OTA_SIGNING_KEY
 *
 * Build: cc -O2 -Ipayload/source -o ota_sign tools/ota_sign.c payload/source/tweetnacl.c
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "tweetnacl.h"

static const unsigned char *g_seed; /* set: randombytes returns this seed (key derivation) */

void randombytes(unsigned char *buf, unsigned long long len) {
    if (g_seed && len == 32) {
        memcpy(buf, g_seed, 32);
        return;
    }
    int fd = open("/dev/urandom", O_RDONLY);
    unsigned long long got = 0;
    if (fd >= 0) {
        while (got < len) {
            ssize_t n = read(fd, buf + got, (size_t)(len - got));
            if (n <= 0) break;
            got += (unsigned long long)n;
        }
        close(fd);
    }
    if (got < len) {
        fprintf(stderr, "ota_sign: cannot read /dev/urandom\n");
        exit(2);
    }
}

static void print_hex(const unsigned char *b, size_t n) {
    for (size_t i = 0; i < n; i++) printf("%02x", b[i]);
}

static int seed_from_env(unsigned char seed[32]) {
    const char *s = getenv("OTA_SIGNING_KEY");
    if (!s) return -1;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    for (int i = 0; i < 32; i++) {
        unsigned int v;
        if (sscanf(s + 2 * i, "%2x", &v) != 1) return -1;
        seed[i] = (unsigned char)v;
    }
    for (const char *p = s + 64; *p; p++) {
        if (*p != ' ' && *p != '\n' && *p != '\r' && *p != '\t') return -1;
    }
    return 0;
}

static void keypair_from_seed(const unsigned char seed[32], unsigned char pk[32], unsigned char sk[64]) {
    g_seed = seed;
    crypto_sign_keypair(pk, sk);
    g_seed = NULL;
}

int main(int argc, char **argv) {
    if (argc >= 2 && strcmp(argv[1], "keygen") == 0) {
        unsigned char seed[32], pk[32], sk[64];
        randombytes(seed, 32);
        keypair_from_seed(seed, pk, sk);
        printf("OTA_SIGNING_KEY (repository secret, keep private):\n");
        print_hex(seed, 32);
        printf("\n\nPublic key line for payload/source/ota_keys.h:\n    { ");
        for (int i = 0; i < 32; i++) printf("0x%02x%s", pk[i], i < 31 ? ", " : "");
        printf(" },\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "pubkey") == 0) {
        unsigned char seed[32], pk[32], sk[64];
        if (seed_from_env(seed) != 0) {
            fprintf(stderr, "ota_sign: OTA_SIGNING_KEY missing or not 64 hex characters\n");
            return 1;
        }
        keypair_from_seed(seed, pk, sk);
        print_hex(pk, 32);
        printf("\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "sign") == 0) {
        unsigned char seed[32], pk[32], sk[64];
        if (seed_from_env(seed) != 0) {
            fprintf(stderr, "ota_sign: OTA_SIGNING_KEY missing or not 64 hex characters\n");
            return 1;
        }
        keypair_from_seed(seed, pk, sk);
        FILE *f = fopen(argv[2], "rb");
        if (!f) {
            perror(argv[2]);
            return 1;
        }
        size_t cap = 1 << 16, len = 0;
        unsigned char *msg = malloc(cap);
        size_t n;
        while (msg && (n = fread(msg + len, 1, cap - len, f)) > 0) {
            len += n;
            if (len == cap) {
                unsigned char *grown = realloc(msg, cap * 2);
                if (!grown) {
                    free(msg);
                    msg = NULL;
                    break;
                }
                msg = grown;
                cap *= 2;
            }
        }
        fclose(f);
        if (!msg) return 1;
        unsigned char *sm = malloc(len + 64);
        unsigned long long smlen = 0;
        if (!sm || crypto_sign(sm, &smlen, msg, len, sk) != 0) return 1;
        print_hex(sm, 64); /* signed message = signature || message */
        printf("\n");
        free(sm);
        free(msg);
        return 0;
    }
    fprintf(stderr, "usage: ota_sign keygen | ota_sign pubkey | ota_sign sign <file>\n");
    return 2;
}
