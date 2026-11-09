/* Factory unit id reader, see mfg_id.h. Shared by hub_daemon and remote_ui. */
#include "mfg_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MFG_MAX_RECORD   4096   /* a value longer than this is not a record chain */
#define MFG_READ_LEN     (MFG_RECORDS_OFF + 8192)

static int parse_from(const uint8_t *p, size_t len, const char *want, char *out, size_t out_len) {
    size_t wlen = strlen(want);
    if (len < MFG_MAGIC_LEN || memcmp(p, MFG_MAGIC, MFG_MAGIC_LEN) != 0) return -1;
    size_t pos = MFG_MAGIC_LEN;
    for (;;) {
        /* key record */
        if (pos + 4 > len) return -1;
        size_t klen = ((size_t)p[pos] << 24) | ((size_t)p[pos + 1] << 16) | ((size_t)p[pos + 2] << 8) | p[pos + 3];
        pos += 4;
        if (klen == 0 || klen > MFG_MAX_RECORD || pos + klen > len) return -1;
        const uint8_t *key = p + pos;
        pos += klen;
        /* value record */
        if (pos + 4 > len) return -1;
        size_t vlen = ((size_t)p[pos] << 24) | ((size_t)p[pos + 1] << 16) | ((size_t)p[pos + 2] << 8) | p[pos + 3];
        pos += 4;
        if (vlen > MFG_MAX_RECORD || pos + vlen > len) return -1;
        const uint8_t *val = p + pos;
        pos += vlen;
        if (klen == wlen && memcmp(key, want, wlen) == 0) {
            /* trim blanks and NULs on both ends */
            size_t a = 0, b = vlen;
            while (a < b && (val[a] == ' ' || val[a] == '\0' || val[a] == '\t' || val[a] == '\n' || val[a] == '\r')) a++;
            while (b > a && (val[b - 1] == ' ' || val[b - 1] == '\0' || val[b - 1] == '\t' || val[b - 1] == '\n' || val[b - 1] == '\r')) b--;
            if (b == a || b - a >= out_len) return -1;
            memcpy(out, val + a, b - a);
            out[b - a] = '\0';
            return 0;
        }
    }
}

int mfg_parse_record(const uint8_t *blob, size_t len, const char *key, char *out, size_t out_len) {
    if (!blob || !key || !key[0] || !out || out_len == 0) return -1;
    out[0] = '\0';
    if (len > MFG_RECORDS_OFF + MFG_MAGIC_LEN &&
        memcmp(blob + MFG_RECORDS_OFF, MFG_MAGIC, MFG_MAGIC_LEN) == 0) {
        return parse_from(blob + MFG_RECORDS_OFF, len - MFG_RECORDS_OFF, key, out, out_len);
    }
    return parse_from(blob, len, key, out, out_len);
}

int mfg_parse_unitid(const uint8_t *blob, size_t len, char *out, size_t out_len) {
    return mfg_parse_record(blob, len, "unitid", out, out_len);
}

int mfg_parse_skin(const uint8_t *blob, size_t len, uint8_t *skin) {
    if (!skin) return -1;
    *skin = 0;
    char txt[16];
    if (mfg_parse_record(blob, len, "skin", txt, sizeof(txt)) != 0) return -1;
    char *end = NULL;
    long v = strtol(txt, &end, 0); /* "0x64" on the Ultimate; a decimal form works too */
    if (!end || end == txt || *end != '\0' || v < 1 || v > 255) return -1;
    *skin = (uint8_t)v;
    return 0;
}

/* /dev/mtdN of the partition named "mfg" in /proc/mtd, or -1 */
static int find_mfg_mtd(char *dev, size_t dev_len) {
    FILE *f = fopen("/proc/mtd", "r");
    if (!f) return -1;
    char line[256];
    int found = -1;
    while (fgets(line, sizeof(line), f)) {
        int n = -1;
        char name[64] = {0};
        /* mtd6: 00010000 00010000 "mfg" */
        if (sscanf(line, "mtd%d: %*s %*s \"%63[^\"]\"", &n, name) == 2 && strcmp(name, "mfg") == 0) {
            found = n;
            break;
        }
    }
    fclose(f);
    if (found < 0) return -1;
    snprintf(dev, dev_len, "/dev/mtd%d", found);
    return 0;
}

/* The partition (or the override file) read into a malloc'd buffer of *n bytes */
static uint8_t *read_partition(const char *override_path, size_t *n) {
    char dev[32];
    const char *path = override_path;
    if (!path || !path[0]) {
        if (find_mfg_mtd(dev, sizeof(dev)) != 0) return NULL;
        path = dev;
    }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t *buf = (uint8_t *)malloc(MFG_READ_LEN);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    *n = fread(buf, 1, MFG_READ_LEN, f);
    fclose(f);
    return buf;
}

int mfg_read_skin(const char *override_path, uint8_t *skin) {
    if (!skin) return -1;
    *skin = 0;
    size_t n = 0;
    uint8_t *buf = read_partition(override_path, &n);
    if (!buf) return -1;
    int rc = mfg_parse_skin(buf, n, skin);
    free(buf);
    return rc;
}

int mfg_read_unitid(const char *override_path, char *out, size_t out_len) {
    if (!out || out_len == 0) return -1;
    out[0] = '\0';
    size_t n = 0;
    uint8_t *buf = read_partition(override_path, &n);
    if (!buf) return -1;
    int rc = mfg_parse_unitid(buf, n, out, out_len);
    free(buf);
    return rc;
}
