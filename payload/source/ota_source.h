#ifndef OTA_SOURCE_H
#define OTA_SOURCE_H

/* The update source: where update.sh, the RF OTA and the Ethernet driver download come from.
 * The update_repo setting (settings.json) overrides it; update.sh keeps its own copy of the same
 * default (DEFAULT_OTA) because it runs without these binaries. */
#define HARMONY_OTA_DEFAULT_URL "https://raw.githubusercontent.com/iranl/harmony-ng/ota"

#include <ctype.h>
#include <string.h>

/* An update source the shell commands can take: http(s):// and a host, under 256 bytes, URL
 * characters only (no quote, space or shell character). Empty is not a source (the default is
 * used). */
static inline int ota_source_url_ok(const char *s) {
    const char *host;
    if (!s || !s[0] || strlen(s) >= 256) return 0;
    if (strncmp(s, "https://", 8) == 0) host = s + 8;
    else if (strncmp(s, "http://", 7) == 0) host = s + 7;
    else return 0;
    if (!*host || *host == '/') return 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c) && !strchr("-_./:@~", c)) return 0;
    }
    return 1;
}

/* The form every consumer reads: no trailing slash and no old "/bin/mips" suffix (update.sh
 * strips both too), so hub_fetch and the API fallbacks build the same URLs. */
static inline void ota_source_normalize(char *s) {
    size_t n = strlen(s);
    while (n && s[n - 1] == '/') s[--n] = '\0';
    if (n >= 9 && strcmp(s + n - 9, "/bin/mips") == 0) s[n -= 9] = '\0';
    while (n && s[n - 1] == '/') s[--n] = '\0';
}

#endif
