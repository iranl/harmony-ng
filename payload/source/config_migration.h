#ifndef CONFIG_MIGRATION_H
#define CONFIG_MIGRATION_H

#ifdef __cplusplus
extern "C" {
#endif

struct cJSON;

void migrate_stock_devices(void);
void migrate_stock_activities(void);
void migrate_activity_ids_to_short(struct cJSON *root);

#ifdef __cplusplus
}
#endif

#endif /* CONFIG_MIGRATION_H */
