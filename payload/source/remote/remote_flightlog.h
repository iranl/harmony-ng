#ifndef HARMONY_REMOTE_FLIGHTLOG_H
#define HARMONY_REMOTE_FLIGHTLOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Flight recorder: a small event log on flash that survives a restart of remote_ui, a reboot, a
 * watchdog reset and a battery pull. /tmp is RAM, so the normal log is gone exactly when it is
 * needed: after the remote stopped responding and only pulling the battery brought it back.
 *
 * Every record is one line, written and fsync()ed before the call returns. The record that
 * announces a suspend is therefore on flash before the kernel is asked to sleep; when the next
 * record in the file is a BOOT, the remote never came back from that suspend.
 *
 * Off unless FLIGHTLOG_ENABLE_FILE exists (touch it over SSH; remove it to stop). Each record
 * costs one small flash write, so this is a diagnostic, not something to leave on for ever. */
#define FLIGHTLOG_FILE        "/data/remote_flightlog.txt"
#define FLIGHTLOG_ENABLE_FILE "/data/flightlog_on"
#define FLIGHTLOG_MAX_BYTES   (1024L * 1024L) /* then the file becomes <file>.1 and a new one starts */

/* Events that open and close a suspend; flightlog_init() reports an open one as unfinished */
#define FLIGHTLOG_EV_SUSPEND_ENTER  "SUSPEND_ENTER"
#define FLIGHTLOG_EV_SUSPEND_RETURN "SUSPEND_RETURN"

bool flightlog_enabled(void);

/* Writes the BOOT record: the version, and what the last record of the previous run was */
void flightlog_init(const char *version);

/* One record: "<local time> up=<s> n=<count> <event> <fmt...> | <context>" */
void flightlog_event(const char *event, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

/* The state appended to every record after " | " (battery, screen, link...): the caller fills buf */
void flightlog_set_context(void (*fn)(char *buf, size_t buf_sz));

/* Copies the newest kernel messages printed since the last call into the log (and clears the
 * kernel's ring buffer, so the next call only sees new ones). The board's pin dump, about 45 lines
 * per suspend, is left out. */
void flightlog_kernel(void);

/* The pieces, for tests */
bool flightlog_last_event(const char *path, char *event, size_t event_sz, char *stamp, size_t stamp_sz);
size_t flightlog_filter_klog(const char *klog, char *out, size_t out_sz, int max_lines);
void flightlog_set_paths(const char *file, const char *enable_file, long max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_REMOTE_FLIGHTLOG_H */
