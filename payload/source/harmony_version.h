#ifndef HARMONY_VERSION_H
#define HARMONY_VERSION_H

/* Version 2.0.0 - Major release.
 * Breaking changes: No backward compatibility with legacy codex naming.
 * Deployment requires force deploy using updated deploy scripts.
 */
#define HARMONY_VERSION "2.0.0"
#define HARMONY_ELITE_VERSION HARMONY_VERSION

#ifndef HARMONY_COMMIT
#define HARMONY_COMMIT "dev"
#endif

#endif /* HARMONY_VERSION_H */
