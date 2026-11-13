#include "ota_verify.h"
#include "ota_keys.h"
#include "sha256.h"
#include "tweetnacl.h"
#include "cJSON.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <strings.h>

#define OTA_MANIFEST_MAX (256 * 1024)

static const unsigned char g_release_keys[][OTA_PUBKEY_BYTES] = {
    OTA_RELEASE_PUBKEYS
#ifdef OTA_EXTRA_PUBKEY
    { OTA_EXTRA_PUBKEY },
#endif
    { 0 } /* terminator, never used as a key */
};

int ota_key_count(void) {
    return (int)(sizeof(g_release_keys) / sizeof(g_release_keys[0])) - 1;
}

/* TweetNaCl needs this symbol for key generation, which the Hub never does. */
void randombytes(unsigned char *buf, unsigned long long len) {
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
    if (got < len) abort(); /* never hand out predictable bytes */
}

int ota_verify_signature(const unsigned char *msg, size_t len, const unsigned char *sig) {
    if (!msg || !sig) return 0;
    int nkeys = ota_key_count();
    if (nkeys <= 0) return 0;
    unsigned long long smlen = (unsigned long long)len + OTA_SIG_BYTES;
    unsigned char *sm = malloc((size_t)smlen);
    unsigned char *m = malloc((size_t)smlen);
    if (!sm || !m) {
        free(sm);
        free(m);
        return 0;
    }
    int ok = 0;
    for (int k = 0; k < nkeys && !ok; k++) {
        memcpy(sm, sig, OTA_SIG_BYTES);
        memcpy(sm + OTA_SIG_BYTES, msg, len);
        unsigned long long mlen = 0;
        if (crypto_sign_open(m, &mlen, sm, smlen, g_release_keys[k]) == 0 && mlen == len) ok = 1;
    }
    free(sm);
    free(m);
    return ok;
}

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int ota_parse_sig_hex(const char *text, unsigned char sig[OTA_SIG_BYTES]) {
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    for (int i = 0; i < OTA_SIG_BYTES; i++) {
        int hi = hexval((unsigned char)text[2 * i]);
        int lo = hi < 0 ? -1 : hexval((unsigned char)text[2 * i + 1]);
        if (hi < 0 || lo < 0) return 0;
        sig[i] = (unsigned char)(hi * 16 + lo);
    }
    for (const char *p = text + 2 * OTA_SIG_BYTES; *p; p++) {
        if (*p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') return 0;
    }
    return 1;
}

static char *read_small_file(const char *path, size_t max, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(max + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, max + 1, f);
    fclose(f);
    if (n == 0 || n > max) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    if (out_len) *out_len = n;
    return buf;
}

char *ota_load_signed_manifest(const char *manifest_path, const char *sig_path, char *err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    size_t mlen = 0;
    char *manifest = read_small_file(manifest_path, OTA_MANIFEST_MAX, &mlen);
    if (!manifest) {
        if (err && err_sz) snprintf(err, err_sz, "manifest missing, empty or too large");
        return NULL;
    }
    char *sigtext = read_small_file(sig_path, 1024, NULL);
    unsigned char sig[OTA_SIG_BYTES];
    if (!sigtext || !ota_parse_sig_hex(sigtext, sig)) {
        free(sigtext);
        free(manifest);
        if (err && err_sz) snprintf(err, err_sz, "manifest signature missing or malformed");
        return NULL;
    }
    free(sigtext);
    if (ota_key_count() <= 0) {
        free(manifest);
        if (err && err_sz) snprintf(err, err_sz, "no release key built into this firmware");
        return NULL;
    }
    if (!ota_verify_signature((const unsigned char *)manifest, mlen, sig)) {
        free(manifest);
        if (err && err_sz) snprintf(err, err_sz, "manifest signature does not match a release key");
        return NULL;
    }
    return manifest;
}

int ota_file_sha256_hex(const char *path, char out_hex[65]) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    sha256_ctx_t ctx;
    sha256_init(&ctx);
    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sha256_update(&ctx, buf, n);
    int failed = ferror(f);
    fclose(f);
    if (failed) return -1;
    unsigned char digest[32];
    sha256_final(digest, &ctx);
    for (int i = 0; i < 32; i++) snprintf(out_hex + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

int ota_manifest_version(const char *manifest_text, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return -1;
    out[0] = '\0';
    cJSON *root = cJSON_Parse(manifest_text);
    if (!root) return -1;
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
    int rc = -1;
    if (cJSON_IsString(v) && v->valuestring && v->valuestring[0]) {
        const char *s = v->valuestring;
        while (*s == 'v' || *s == 'V') s++;
        int good = s[0] != '\0';
        for (const char *p = s; *p; p++) {
            if (!((*p >= '0' && *p <= '9') || *p == '.')) good = 0;
        }
        if (good && strlen(s) < out_sz) {
            strcpy(out, s);
            rc = 0;
        }
    }
    cJSON_Delete(root);
    return rc;
}

int ota_manifest_check_file(const char *manifest_text, const char *key, const char *path, char *err, size_t err_sz) {
    if (err && err_sz) err[0] = '\0';
    cJSON *root = cJSON_Parse(manifest_text);
    if (!root) {
        if (err && err_sz) snprintf(err, err_sz, "manifest is not valid JSON");
        return -1;
    }
    cJSON *files = cJSON_GetObjectItemCaseSensitive(root, "files");
    cJSON *entry = cJSON_IsObject(files) ? cJSON_GetObjectItemCaseSensitive(files, key) : NULL;
    cJSON *sha = cJSON_IsObject(entry) ? cJSON_GetObjectItemCaseSensitive(entry, "sha256") : NULL;
    if (!cJSON_IsString(sha) || !sha->valuestring || strlen(sha->valuestring) != 64) {
        if (err && err_sz) snprintf(err, err_sz, "%s: no SHA-256 in the manifest", key);
        cJSON_Delete(root);
        return -1;
    }
    char actual[65];
    int rc = -1;
    if (ota_file_sha256_hex(path, actual) != 0) {
        if (err && err_sz) snprintf(err, err_sz, "%s: cannot read %s", key, path);
    } else if (strcasecmp(actual, sha->valuestring) != 0) {
        if (err && err_sz) snprintf(err, err_sz, "%s: SHA-256 mismatch", key);
    } else {
        rc = 0;
    }
    cJSON_Delete(root);
    return rc;
}
