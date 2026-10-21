#ifndef CODEX_NTP_H
#define CODEX_NTP_H

#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Queries SNTP server and updates system clock via settimeofday() if apply != 0.
 * Returns unix epoch on success, 0 on failure.
 */
time_t sntp_sync_time(const char *server, int apply);

/*
 * Periodic tick called from main event loop.
 * Automatically syncs on boot and retries until valid time is acquired,
 * then periodically re-syncs every 4 hours.
 */
void sntp_tick(const char *server);

#ifdef __cplusplus
}
#endif

#endif /* CODEX_NTP_H */
