#ifndef HARMONY_VERSION_H
#define HARMONY_VERSION_H

#include <stdlib.h>
#include <string.h>

/* Version 2.0.0 - Major release.
 * Breaking changes: No backward compatibility with legacy codex naming.
 * Deployment requires force deploy using updated deploy scripts.
 */
#define HARMONY_VERSION "2.0.0"
#define HARMONY_ELITE_VERSION HARMONY_VERSION

#ifndef HARMONY_COMMIT
#define HARMONY_COMMIT "dev"
#endif

/**
 * Compare two semantic version strings (e.g. "2.0.0", "v2.0.1", "1.9.9", "unknown").
 * Returns:
 *   > 0 if v1 > v2
 *     0 if v1 == v2
 *   < 0 if v1 < v2
 */
static inline int harmony_version_compare(const char *v1, const char *v2) {
    if (!v1 && !v2) return 0;
    if (!v1) return -1;
    if (!v2) return 1;
    while (*v1 == 'v' || *v1 == 'V' || *v1 == ' ') v1++;
    while (*v2 == 'v' || *v2 == 'V' || *v2 == ' ') v2++;
    if (strcmp(v1, "unknown") == 0) return (strcmp(v2, "unknown") == 0) ? 0 : -1;
    if (strcmp(v2, "unknown") == 0) return 1;

    const char *p1 = v1;
    const char *p2 = v2;
    while (*p1 || *p2) {
        long n1 = 0, n2 = 0;
        char *end1 = NULL;
        char *end2 = NULL;
        if (*p1) {
            n1 = strtol(p1, &end1, 10);
            p1 = end1;
            while (*p1 && *p1 != '.') p1++;
            if (*p1 == '.') p1++;
        }
        if (*p2) {
            n2 = strtol(p2, &end2, 10);
            p2 = end2;
            while (*p2 && *p2 != '.') p2++;
            if (*p2 == '.') p2++;
        }
        if (n1 > n2) return 1;
        if (n1 < n2) return -1;
    }
    return 0;
}

#endif /* HARMONY_VERSION_H */

