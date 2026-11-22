#define _GNU_SOURCE
#include "icon_pack.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Only the raw deflate decoder of stb_image is used */
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_FAILURE_STRINGS
#define STBI_NO_SIMD
#define STBI_ASSERT(x) ((void)0)
#if defined(__GNUC__)
#pragma GCC diagnostic ignored "-Wunused-function" /* the image loaders stb_image declares */
#endif
#include "stb_image.h"

#ifndef ICON_PACK_ARCHIVES
#define ICON_PACK_ARCHIVES { "/data/harmony/icons.tar.gz", "/cache/icons.tar.gz", "/cache/www/assets/icons.tar.gz" }
#endif

/* gzip header written by tools/icon_pack.py: 10 bytes, XLEN, subfield 'H' 'I' with 20 bytes */
#define PACK_HEADER_LEN 36
#define PACK_VERSION 1
#define PACK_MAX_INDEX (1024 * 1024)
#define PACK_MAX_PART (4 * 1024 * 1024)
/* A part is inflated as a whole stream: a final empty fixed-Huffman block (0x03 0x00) ends it,
 * and zero bytes after it keep the decoder's read-ahead inside the buffer */
static const unsigned char PART_END[] = { 0x03, 0x00, 0, 0, 0, 0, 0, 0, 0, 0 };

struct pack_part { uint32_t off, comp_len, raw_len; };
struct pack_entry { const char *name; uint32_t part, off, size; };

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_path[256];
static struct stat s_st;      /* of s_path when the index was read (or found missing) */
static int s_looked;
static char *s_index;         /* index text; entry names point into it */
static struct pack_part *s_parts;
static size_t s_nparts;
static struct pack_entry *s_entries;
static size_t s_nentries;

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void drop_index(void) {
    free(s_index);
    free(s_parts);
    free(s_entries);
    s_index = NULL;
    s_parts = NULL;
    s_entries = NULL;
    s_nparts = s_nentries = 0;
}

static int read_at(int fd, void *buf, size_t len, off_t off) {
    size_t got = 0;
    while (got < len) {
        ssize_t n = pread(fd, (char *)buf + got, len - got, off + (off_t)got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 0;
}

/* The tar bytes of the part that starts at off (malloc); NULL on any error */
static unsigned char *inflate_part(int fd, uint32_t off, uint32_t comp_len, uint32_t raw_len) {
    if (comp_len == 0 || raw_len == 0 || raw_len > PACK_MAX_PART || comp_len > PACK_MAX_PART) return NULL;
    unsigned char *in = malloc((size_t)comp_len + sizeof(PART_END));
    unsigned char *out = malloc(raw_len);
    if (!in || !out || read_at(fd, in, comp_len, (off_t)off) != 0) {
        free(in);
        free(out);
        return NULL;
    }
    memcpy(in + comp_len, PART_END, sizeof(PART_END));
    int n = stbi_zlib_decode_noheader_buffer((char *)out, (int)raw_len, (const char *)in,
                                             (int)(comp_len + sizeof(PART_END)));
    free(in);
    if (n != (int)raw_len) {
        free(out);
        return NULL;
    }
    return out;
}

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const struct pack_entry *)a)->name, ((const struct pack_entry *)b)->name);
}

/* Reads the index of the archive open on fd (size bytes); 0 when it is a usable pack */
static int load_index(int fd, off_t size) {
    unsigned char h[PACK_HEADER_LEN];
    if (size < PACK_HEADER_LEN + 8 || read_at(fd, h, sizeof(h), 0) != 0) return -1;
    if (h[0] != 0x1f || h[1] != 0x8b || h[2] != 8 || h[3] != 0x04) return -1; /* FEXTRA only */
    if (h[10] != 24 || h[11] != 0 || h[12] != 'H' || h[13] != 'I' || h[14] != 20 || h[15] != 0) return -1;
    if (h[16] != PACK_VERSION) return -1;
    uint32_t idx_off = le32(h + 20), idx_comp = le32(h + 24), idx_raw = le32(h + 28), idx_size = le32(h + 32);
    if (idx_off < PACK_HEADER_LEN || (off_t)idx_off + (off_t)idx_comp + 8 != size) return -1;
    if (idx_raw > PACK_MAX_INDEX || idx_size == 0 || (uint64_t)idx_size + 512 > idx_raw) return -1;

    unsigned char *raw = inflate_part(fd, idx_off, idx_comp, idx_raw);
    if (!raw) return -1;
    char *text = malloc((size_t)idx_size + 1);
    if (!text) {
        free(raw);
        return -1;
    }
    memcpy(text, raw + 512, idx_size);
    text[idx_size] = '\0';
    free(raw);

    size_t lines = 0;
    for (const char *p = text; *p; p++) lines += (*p == '\n');
    struct pack_part *parts = calloc(lines + 1, sizeof(*parts));
    struct pack_entry *entries = calloc(lines + 1, sizeof(*entries));
    size_t np = 0, ne = 0;
    int ok = parts && entries && strncmp(text, "HIP1\n", 5) == 0;
    for (char *line = text + 5; ok && *line;) {
        char *nl = strchr(line, '\n');
        if (!nl) { ok = 0; break; }
        *nl = '\0';
        unsigned a, b, c;
        int used = 0;
        if (line[0] == 'S' && sscanf(line, "S %u %u %u%n", &a, &b, &c, &used) == 3 && line[used] == '\0') {
            ok = a >= PACK_HEADER_LEN && (uint64_t)a + b <= idx_off && c <= PACK_MAX_PART && ne == 0;
            parts[np++] = (struct pack_part){ a, b, c };
        } else if (line[0] == 'F' && sscanf(line, "F %u %u %u %n", &a, &b, &c, &used) == 3 && line[used]) {
            ok = a < np && (uint64_t)b + c <= parts[a].raw_len;
            entries[ne++] = (struct pack_entry){ line + used, a, b, c };
        } else {
            ok = 0;
        }
        line = nl + 1;
    }
    if (!ok || ne == 0) {
        free(text);
        free(parts);
        free(entries);
        return -1;
    }
    qsort(entries, ne, sizeof(*entries), entry_cmp);
    s_index = text;
    s_parts = parts;
    s_nparts = np;
    s_entries = entries;
    s_nentries = ne;
    return 0;
}

/* Brings the index up to date with the archive on flash; called with s_lock held */
static void refresh_locked(void) {
    static const char *const paths[] = ICON_PACK_ARCHIVES;
    const char *path = NULL;
    struct stat st;
    for (size_t i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        if (stat(paths[i], &st) == 0 && S_ISREG(st.st_mode)) {
            path = paths[i];
            break;
        }
    }
    if (!path) {
        if (s_looked && !s_path[0]) return;
        drop_index();
        s_path[0] = '\0';
        s_looked = 1;
        return;
    }
    if (s_looked && strcmp(path, s_path) == 0 && st.st_ino == s_st.st_ino && st.st_size == s_st.st_size &&
        st.st_mtime == s_st.st_mtime) {
        return;
    }
    drop_index();
    snprintf(s_path, sizeof(s_path), "%s", path);
    s_st = st;
    s_looked = 1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    if (load_index(fd, st.st_size) == 0) {
        printf("[+] Icons read from %s (%zu names, %zu parts)\n", path, s_nentries, s_nparts);
    }
    close(fd);
}

static const struct pack_entry *find_locked(const char *name) {
    struct pack_entry key = { name, 0, 0, 0 };
    return s_nentries ? bsearch(&key, s_entries, s_nentries, sizeof(key), entry_cmp) : NULL;
}

int icon_pack_available(void) {
    pthread_mutex_lock(&s_lock);
    refresh_locked();
    int ok = s_nentries > 0;
    pthread_mutex_unlock(&s_lock);
    return ok;
}

int icon_pack_has(const char *name) {
    if (!name || !name[0]) return 0;
    pthread_mutex_lock(&s_lock);
    refresh_locked();
    int ok = find_locked(name) != NULL;
    pthread_mutex_unlock(&s_lock);
    return ok;
}

unsigned char *icon_pack_read(const char *name, size_t *len) {
    if (!name || !name[0]) return NULL;
    pthread_mutex_lock(&s_lock);
    refresh_locked();
    const struct pack_entry *e = find_locked(name);
    struct pack_entry ent = e ? *e : (struct pack_entry){ 0 };
    struct pack_part part = e ? s_parts[e->part] : (struct pack_part){ 0 };
    char path[sizeof(s_path)];
    snprintf(path, sizeof(path), "%s", s_path);
    pthread_mutex_unlock(&s_lock);
    if (!e) return NULL;

    /* inflated outside the lock: the values were copied, a reload frees nothing used here */
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    unsigned char *raw = inflate_part(fd, part.off, part.comp_len, part.raw_len);
    close(fd);
    if (!raw) return NULL;
    unsigned char *data = malloc(ent.size ? ent.size : 1);
    if (data) {
        memcpy(data, raw + ent.off, ent.size);
        if (len) *len = ent.size;
    }
    free(raw);
    return data;
}

void icon_pack_list(void (*cb)(const char *name, void *ctx), void *ctx) {
    if (!cb) return;
    pthread_mutex_lock(&s_lock);
    refresh_locked();
    for (size_t i = 0; i < s_nentries; i++) {
        if (!strchr(s_entries[i].name, '/')) cb(s_entries[i].name, ctx);
    }
    pthread_mutex_unlock(&s_lock);
}
