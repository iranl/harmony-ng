#include "remote_migrate.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char *old_name;
    const char *new_name;
    int binary; /* a regular file under the old name replaces the new one */
} legacy_name_t;

static const legacy_name_t k_names[] = {
    { "elite_ui", "remote_ui", 1 },
    { "elite_sync", "remote_sync", 1 },
    { "elite_watchdog.sh", "remote_watchdog.sh", 0 },
    { "elite_settings.conf", "remote_settings.conf", 0 },
    { "elite_ui.conf", "remote_ui.conf", 0 },
    { "elite_rf_config.json", "remote_rf_config.json", 0 },
    { "elite_activity_usage.conf", "remote_activity_usage.conf", 0 },
    { "elite_ui.bak", "remote_ui.bak", 0 },
    { NULL, NULL, 0 }
};

int remote_migrate_legacy(const char *data_dir) {
    if (!data_dir) return 0;
    int moved = 0;
    char o[256], n[256];
    struct stat st;
    for (int i = 0; k_names[i].old_name; i++) {
        snprintf(o, sizeof(o), "%s/%s", data_dir, k_names[i].old_name);
        snprintf(n, sizeof(n), "%s/%s", data_dir, k_names[i].new_name);
        int old_is_file = (lstat(o, &st) == 0 && S_ISREG(st.st_mode));
        int new_exists = (stat(n, &st) == 0);
        if (old_is_file && (k_names[i].binary || !new_exists)) {
            if (rename(o, n) == 0) {
                if (k_names[i].binary) chmod(n, 0755);
                moved++;
                new_exists = 1;
            }
        }
        /* the old name points at the new file (relative, inside the same directory) */
        if (new_exists && lstat(o, &st) != 0) {
            if (symlink(k_names[i].new_name, o) != 0) { /* read-only or full: nothing to do */ }
        }
    }
    snprintf(o, sizeof(o), "%s/elite_conf_p", data_dir);
    snprintf(n, sizeof(n), "%s/remote_conf_p", data_dir);
    if (lstat(o, &st) == 0 && S_ISDIR(st.st_mode) && stat(n, &st) != 0) {
        if (rename(o, n) == 0) moved++;
    }
    if (moved) {
        printf("[*] Migrated %d file(s) from %s/elite_* to %s/remote_*\n", moved, data_dir, data_dir);
        sync();
    }
    return moved;
}
