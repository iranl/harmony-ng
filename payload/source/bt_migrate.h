#ifndef HARMONY_BT_MIGRATE_H
#define HARMONY_BT_MIGRATE_H

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*bt_migrate_log_fn)(const char *msg);

/* One-time move of the legacy Bluetooth state files in harmony_dir (normally /data/harmony)
 * into config/devices.json and bthid_host_target. Safe to call repeatedly: files that are
 * missing, already migrated or unreadable are left alone. log_fn may be NULL. hub_daemon owns
 * devices.json and caches it, so only hub_daemon calls this (config_store_init). */
void bt_migrate_legacy_state(const char *harmony_dir, bt_migrate_log_fn log_fn);

#ifdef __cplusplus
}
#endif

#endif /* HARMONY_BT_MIGRATE_H */
