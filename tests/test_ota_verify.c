/* Signed OTA manifests (ota_verify.c, TweetNaCl Ed25519).
 * Build with the test release key: -DOTA_EXTRA_PUBKEY=$(cat tests/ota_test_pubkey.txt)
 * The test key's secret seed is the bytes 1..32; it signs nothing outside the tests. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "ota_verify.h"
#include "tweetnacl.h"

static int failed;
#define CHECK(c, name) do { if (c) printf("  PASS %s\n", name); else { printf("  FAIL %s\n", name); failed++; } } while (0)

static void unhex(const char *h, unsigned char *out, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        sscanf(h + 2 * i, "%2x", &v);
        out[i] = (unsigned char)v;
    }
}

static void write_file(const char *path, const void *data, size_t len) {
    FILE *f = fopen(path, "wb");
    fwrite(data, 1, len, f);
    fclose(f);
}

/* Sign msg with seed||pk (TweetNaCl secret key layout), write the signature as hex. */
static void sign_to_file(const unsigned char sk[64], const char *msg, size_t len, const char *sig_path) {
    unsigned char *sm = malloc(len + 64);
    unsigned long long smlen;
    crypto_sign(sm, &smlen, (const unsigned char *)msg, len, sk);
    FILE *f = fopen(sig_path, "w");
    for (int i = 0; i < 64; i++) fprintf(f, "%02x", sm[i]);
    fprintf(f, "\n");
    fclose(f);
    free(sm);
}

static int rfc_vector(const char *pk_hex, const char *sig_hex, const unsigned char *msg, size_t len) {
    unsigned char pk[32], sm[64 + 8], m[64 + 8];
    unsigned long long mlen;
    unhex(pk_hex, pk, 32);
    unhex(sig_hex, sm, 64);
    memcpy(sm + 64, msg, len);
    return crypto_sign_open(m, &mlen, sm, 64 + len, pk) == 0 && mlen == len;
}

int main(void) {
    char dir[] = "/tmp/ota_verify_testXXXXXX";
    if (!mkdtemp(dir)) return 1;
    char mpath[256], spath[256], fpath[256];
    snprintf(mpath, sizeof(mpath), "%s/manifest.json", dir);
    snprintf(spath, sizeof(spath), "%s/manifest.json.sig", dir);
    snprintf(fpath, sizeof(fpath), "%s/hub_ota.tar.gz", dir);
    char err[160];

    printf("[-] Ed25519 (RFC 8032 section 7.1, tests 1 and 2)\n");
    CHECK(rfc_vector("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
                     "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                     (const unsigned char *)"", 0), "RFC 8032 test 1 verifies");
    const unsigned char m72 = 0x72;
    CHECK(rfc_vector("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                     "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
                     &m72, 1), "RFC 8032 test 2 verifies");
    const unsigned char m73 = 0x73;
    CHECK(!rfc_vector("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
                      "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
                      &m73, 1), "RFC 8032 test 2 with another message is refused");

    /* test release key: seed 1..32 */
    unsigned char sk[64];
    for (int i = 0; i < 32; i++) sk[i] = (unsigned char)(i + 1);
    unhex("79b5562e8fe654f94078b112e8a98ba7901f853ae695bed7e0e3910bad049664", sk + 32, 32);
    /* another key (RFC 8032 test 1) that is not a release key */
    unsigned char other[64];
    unhex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60", other, 32);
    unhex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", other + 32, 32);

    const char payload[] = "hub package bytes\n";
    write_file(fpath, payload, sizeof(payload) - 1);
    char sha[65];
    ota_file_sha256_hex(fpath, sha);
    char manifest[512];
    int mlen = snprintf(manifest, sizeof(manifest),
                        "{\"version\":\"v2.5.0\",\"files\":{\"hub_ota.tar.gz\":{\"size\":%zu,\"sha256\":\"%s\"}}}",
                        sizeof(payload) - 1, sha);

    printf("[-] Signed manifest\n");
    CHECK(ota_key_count() == 1, "one release key built in (the test key)");
    write_file(mpath, manifest, (size_t)mlen);
    sign_to_file(sk, manifest, (size_t)mlen, spath);
    char *text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text != NULL, "manifest signed with the release key is accepted");
    char ver[32];
    CHECK(text && ota_manifest_version(text, ver, sizeof(ver)) == 0 && strcmp(ver, "2.5.0") == 0, "version read without the v");
    CHECK(text && ota_manifest_check_file(text, "hub_ota.tar.gz", fpath, err, sizeof(err)) == 0, "file matching its SHA-256 accepted");
    CHECK(text && ota_manifest_check_file(text, "www.tar.gz", fpath, err, sizeof(err)) != 0, "file without a manifest entry refused");
    write_file(fpath, "other bytes", 11);
    CHECK(text && ota_manifest_check_file(text, "hub_ota.tar.gz", fpath, err, sizeof(err)) != 0 && strstr(err, "mismatch"),
          "file with another SHA-256 refused");
    free(text);

    char tampered[512];
    memcpy(tampered, manifest, (size_t)mlen);
    tampered[13] = '9'; /* "v2.5.0" -> "v9.5.0" */
    write_file(mpath, tampered, (size_t)mlen);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text == NULL && strstr(err, "does not match"), "manifest changed after signing refused");
    free(text);

    write_file(mpath, manifest, (size_t)mlen);
    sign_to_file(other, manifest, (size_t)mlen, spath);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text == NULL, "manifest signed with another key refused");
    free(text);

    write_file(spath, "abcd\n", 5);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text == NULL && strstr(err, "malformed"), "short signature refused");
    free(text);

    sign_to_file(sk, manifest, (size_t)mlen, spath);
    FILE *f = fopen(spath, "a");
    fputs("junk", f);
    fclose(f);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text == NULL, "signature with trailing junk refused");
    free(text);

    unlink(spath);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text == NULL && strstr(err, "missing"), "missing signature refused");
    free(text);

    const char *bad = "{\"version\":\"2.5.0;reboot\",\"files\":{}}";
    write_file(mpath, bad, strlen(bad));
    sign_to_file(sk, bad, strlen(bad), spath);
    text = ota_load_signed_manifest(mpath, spath, err, sizeof(err));
    CHECK(text && ota_manifest_version(text, ver, sizeof(ver)) != 0, "version with other characters refused");
    free(text);

    unlink(mpath);
    unlink(spath);
    unlink(fpath);
    rmdir(dir);
    if (failed) {
        printf("[-] %d check(s) failed\n", failed);
        return 1;
    }
    printf("[+] ota_verify: all passed\n");
    return 0;
}
