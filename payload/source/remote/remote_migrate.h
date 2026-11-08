#ifndef HARMONY_REMOTE_MIGRATE_H
#define HARMONY_REMOTE_MIGRATE_H

/*
 * Builds before the elite -> remote rename kept their files as <data>/elite_* (binary, sync
 * helper, watchdog script, settings, RF config, activity usage, factory-reset backup). This moves
 * them to <data>/remote_* and leaves the old names as relative links, so a supervisor or installer
 * of an older build that is still running, or a rollback to one, finds them. Idempotent.
 *
 * A regular file under an old binary name is a binary an older installer has just put there (it
 * looks for that name): it replaces the new one. Settings and the like move only when the new
 * file does not exist yet. The same steps are in remote/init.sh ("init.sh migrate"), which runs
 * them at boot before this binary starts; remote_main.c runs this first thing, before any config
 * is read, because an older installer may start this binary directly as <data>/elite_ui.
 *
 * Returns the number of files moved.
 */
int remote_migrate_legacy(const char *data_dir);

#endif
