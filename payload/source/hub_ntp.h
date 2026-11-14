#ifndef HUB_NTP_H
#define HUB_NTP_H

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

/*
 * The two halves of sntp_tick() for a caller that runs the blocking sntp_sync_time() on
 * another thread: sntp_sync_due() says whether a sync should start now (and counts it as
 * started), sntp_sync_done() takes its result. Call them from one thread at a time.
 */
int sntp_sync_due(void);
void sntp_sync_done(time_t res);

#ifdef __cplusplus
}
#endif

#endif /* HUB_NTP_H */
