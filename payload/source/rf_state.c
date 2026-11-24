#define _GNU_SOURCE
#include "rf_state.h"
#include "rf_proto.h"
#include "cJSON.h"
#include "atomic_file.h"
#include "rf_slot_table.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/select.h>

volatile bool g_running = true;
/* POST /api/rf/restart: exit once idle, init.sh starts hub_rf again */
volatile bool g_restart_requested = false;
int g_rf_fd = -1;
pthread_mutex_t g_rf_mutex = PTHREAD_MUTEX_INITIALIZER;
uint8_t g_paired_addr[4] = {0};
uint8_t g_hub_rf_addr[4] = {0};
uint8_t g_remote_slot = 0x01;
bool g_has_paired_device = false;
uint8_t g_fw_version = 0;
volatile bool g_pairing_mode = false;
volatile bool g_config_push_running = false;
uint8_t g_config_version = 1;
int g_log_level = LOG_LEVEL_INFO;
int g_debug_logging = 0;

remote_battery_t g_remote_batteries[MAX_PAIRED_REMOTES];

const char *g_rf_battery_cache_dir = "/tmp";
/* Where remote_slot<N>_version.json lives (a test points it elsewhere) */
const char *g_rf_version_dir = "/data/harmony";
time_t (*g_rf_time_fn)(time_t *) = time;

/* Before this the clock has not been set: 2024-01-01 */
#define RF_CLOCK_VALID_EPOCH 1704067200LL

_Static_assert(MAX_PAIRED_REMOTES == 6, "slot initializers below list 6 slots");

#define RV_UNKNOWN { .sw_version = "unknown", .config_version = 0, .last_report_ms = 0 }
remote_version_t g_remote_versions[MAX_PAIRED_REMOTES] = {
    RV_UNKNOWN, RV_UNKNOWN, RV_UNKNOWN, RV_UNKNOWN, RV_UNKNOWN, RV_UNKNOWN
};
#undef RV_UNKNOWN

remote_link_stats_t g_remote_link[MAX_PAIRED_REMOTES];

paired_remote_t g_paired_remotes[MAX_PAIRED_REMOTES] = {
    { .slot = 0x01 }, { .slot = 0x02 }, { .slot = 0x03 }, { .slot = 0x04 }, { .slot = 0x05 }, { .slot = 0x06 }
};

rf_ota_state_t g_rf_ota = { false, 0, 0, 0, 0, 0, 0, "" };
pthread_mutex_t g_ota_lock = PTHREAD_MUTEX_INITIALIZER;

/* Rename <old> to <new> unless <new> exists; 1 when it moved */
static int rf_migrate_one(const char *old_path, const char *new_path) {
    if (access(old_path, F_OK) != 0 || access(new_path, F_OK) == 0) return 0;
    if (rename(old_path, new_path) != 0) return 0;
    printf("[*] Migrated %s -> %s\n", old_path, new_path);
    return 1;
}

int rf_migrate_legacy_names_in(const char *harmony_dir, const char *cache_dir) {
    char o[160], n[160];
    int moved = 0;
    for (int slot = 1; slot <= MAX_PAIRED_REMOTES; slot++) {
        static const char *const kinds[] = { "version", "config", NULL };
        for (int k = 0; kinds[k]; k++) {
            snprintf(o, sizeof(o), "%s/elite_slot%d_%s.json", harmony_dir, slot, kinds[k]);
            snprintf(n, sizeof(n), "%s/remote_slot%d_%s.json", harmony_dir, slot, kinds[k]);
            moved += rf_migrate_one(o, n);
        }
    }
    /* Migrate version metadata */
    snprintf(o, sizeof(o), "%s/elite_ui.version", harmony_dir);
    snprintf(n, sizeof(n), "%s/remote_ui.version", harmony_dir);
    moved += rf_migrate_one(o, n);

    /* Purge legacy OTA binaries from flash storage (OTA images reside only in /tmp RAM) */
    snprintf(o, sizeof(o), "%s/elite_ui.ota.gz", harmony_dir);
    unlink(o);
    snprintf(o, sizeof(o), "%s/remote_ui.ota.gz", harmony_dir);
    unlink(o);
    if (cache_dir) {
        snprintf(o, sizeof(o), "%s/elite_ui.ota.gz", cache_dir);
        unlink(o);
        snprintf(o, sizeof(o), "%s/remote_ui.ota.gz", cache_dir);
        unlink(o);
    }
    if (moved) sync();
    return moved;
}

void rf_migrate_legacy_names(void) {
    rf_migrate_legacy_names_in("/data/harmony", "/cache");
}

uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

static void battery_cache_path(int slot, char *out, size_t len) {
    snprintf(out, len, "%s/hub_rf_battery_slot%d.json", g_rf_battery_cache_dir, slot);
}

static int64_t wall_now(void) {
    int64_t t = (int64_t)g_rf_time_fn(NULL);
    return t >= RF_CLOCK_VALID_EPOCH ? t : 0;
}

static void battery_cache_write(int slot) {
    const remote_battery_t *b = &g_remote_batteries[slot - 1];
    char path[160], tmp[176];
    battery_cache_path(slot, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "{\"percent\":%u,\"charging\":%s,\"voltage_mv\":%u,\"reported_at\":%lld,\"mono_ms\":%llu}\n",
            (unsigned)b->percent, b->charging ? "true" : "false", (unsigned)b->voltage_mv,
            (long long)b->reported_at, (unsigned long long)b->last_update_ms);
    fclose(f);
    rename(tmp, path);
}

void rf_battery_note_report(uint8_t slot, uint8_t percent, uint8_t charging, uint16_t voltage_mv) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    remote_battery_t *b = &g_remote_batteries[slot - 1];
    b->percent = percent;
    b->charging = charging;
    b->voltage_mv = voltage_mv;
    b->last_update_ms = get_monotonic_ms();
    b->reported_at = wall_now();
    b->restored = 0;
    battery_cache_write(slot);
}

void rf_battery_restore(void) {
    uint64_t now = get_monotonic_ms();
    for (int slot = 1; slot <= MAX_PAIRED_REMOTES; slot++) {
        char path[160];
        battery_cache_path(slot, path, sizeof(path));
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char buf[256] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        cJSON *j = cJSON_Parse(buf);
        if (!j) continue;
        const cJSON *pc = cJSON_GetObjectItem(j, "percent");
        const cJSON *ch = cJSON_GetObjectItem(j, "charging");
        const cJSON *mv = cJSON_GetObjectItem(j, "voltage_mv");
        const cJSON *ra = cJSON_GetObjectItem(j, "reported_at");
        const cJSON *mo = cJSON_GetObjectItem(j, "mono_ms");
        /* mono_ms from a later time than now: written in another boot (should not happen in /tmp) */
        if (cJSON_IsNumber(pc) && cJSON_IsNumber(mv) && cJSON_IsNumber(mo) && mo->valuedouble > 0 &&
            (uint64_t)mo->valuedouble <= now) {
            remote_battery_t *b = &g_remote_batteries[slot - 1];
            b->percent = (uint8_t)pc->valueint;
            b->charging = cJSON_IsTrue(ch) ? 1 : 0;
            b->voltage_mv = (uint16_t)mv->valueint;
            b->last_update_ms = (uint64_t)mo->valuedouble;
            b->reported_at = cJSON_IsNumber(ra) ? (int64_t)ra->valuedouble : 0;
            b->restored = 1;
            printf("[+] Battery of slot %d restored from %s: %u%% (%us old)\n", slot, path,
                   (unsigned)b->percent, (unsigned)((now - b->last_update_ms) / 1000));
        }
        cJSON_Delete(j);
    }
    rf_battery_fix_times();
}

void rf_slot_forget(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    int i = slot - 1;
    paired_remote_t *p = &g_paired_remotes[i];
    p->active = false;
    p->heard = false;
    memset(p->rf_addr, 0, sizeof(p->rf_addr));
    p->name[0] = '\0';
    p->equad_id = 0;
    p->model = REMOTE_MODEL_UNKNOWN;
    remote_version_t *v = &g_remote_versions[i];
    snprintf(v->sw_version, sizeof(v->sw_version), "%s", "unknown");
    v->config_version = 0;
    v->caps = 0;
    v->last_report_ms = 0;
    v->remote_id[0] = '\0';
    v->skin = 0;
    memset(&g_remote_batteries[i], 0, sizeof(g_remote_batteries[i]));
    char path[160];
    battery_cache_path(slot, path, sizeof(path));
    unlink(path);
    rf_state_save_version_file(slot);
}

int rf_battery_fix_times(void) {
    int64_t wall = wall_now();
    if (!wall) return 0;
    uint64_t now = get_monotonic_ms();
    int changed = 0;
    for (int slot = 1; slot <= MAX_PAIRED_REMOTES; slot++) {
        remote_battery_t *b = &g_remote_batteries[slot - 1];
        if (b->last_update_ms && !b->reported_at && b->last_update_ms <= now) {
            b->reported_at = wall - (int64_t)((now - b->last_update_ms) / 1000);
            battery_cache_write(slot);
            changed = 1;
        }
    }
    return changed;
}

void mark_remote_slot_active(uint8_t slot) {
    if (slot >= 1 && slot <= MAX_PAIRED_REMOTES) {
        int idx = slot - 1;
        g_paired_remotes[idx].active = true;
        g_paired_remotes[idx].link_lost = false;
        g_paired_remotes[idx].last_seen_ms = get_monotonic_ms();
        g_remote_slot = slot;
        g_has_paired_device = true;
    }
}

const char *g_rf_slot_map_path = "/data/harmony/rf_slot_map.txt";
static uint8_t s_pipe_of_slot[MAX_PAIRED_REMOTES + 1]; /* [slot] = pipe; all 0 until first use */
static uint8_t s_slot_of_pipe[MAX_PAIRED_REMOTES + 1]; /* [pipe] = slot */
static pthread_mutex_t s_slot_map_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t s_pair_target;            /* 0: no chosen slot */
static uint64_t s_pair_target_until_ms;  /* the chosen slot is dropped after this */

/* s_slot_map_lock held */
static void slot_map_set_locked(const uint8_t *pipes) {
    for (int s = 1; s <= MAX_PAIRED_REMOTES; s++) {
        s_pipe_of_slot[s] = pipes[s - 1];
        s_slot_of_pipe[pipes[s - 1]] = (uint8_t)s;
    }
}

/* s_slot_map_lock held */
static void slot_map_ensure_locked(void) {
    if (s_pipe_of_slot[1] != 0) return;
    uint8_t same[MAX_PAIRED_REMOTES];
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) same[i] = (uint8_t)(i + 1);
    slot_map_set_locked(same);
}

uint8_t rf_slot_to_pipe(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return slot;
    pthread_mutex_lock(&s_slot_map_lock);
    slot_map_ensure_locked();
    uint8_t pipe = s_pipe_of_slot[slot];
    pthread_mutex_unlock(&s_slot_map_lock);
    return pipe;
}

uint8_t rf_pipe_to_slot(uint8_t pipe) {
    if (pipe < 1 || pipe > MAX_PAIRED_REMOTES) return pipe;
    pthread_mutex_lock(&s_slot_map_lock);
    slot_map_ensure_locked();
    uint8_t slot = s_slot_of_pipe[pipe];
    pthread_mutex_unlock(&s_slot_map_lock);
    return slot;
}

_Static_assert(MAX_PAIRED_REMOTES == RF_SLOT_TABLE_SLOTS, "the slot table has one pipe per slot");

void rf_slot_map_load(void) {
    uint8_t pipes[MAX_PAIRED_REMOTES];
    /* no file: slots and pipes the same; a file that is no table: the same, said once */
    if (!rf_slot_table_read(g_rf_slot_map_path, pipes) && access(g_rf_slot_map_path, F_OK) == 0)
        printf("[-] %s is not a slot table, slots and pipes are the same\n", g_rf_slot_map_path);
    pthread_mutex_lock(&s_slot_map_lock);
    slot_map_set_locked(pipes);
    pthread_mutex_unlock(&s_slot_map_lock);
    for (int s = 1; s <= MAX_PAIRED_REMOTES; s++)
        if (pipes[s - 1] != s) printf("[*] Slot %d is CC2544 pipe %d\n", s, pipes[s - 1]);
}

/* A slot holds nothing worth a file: what rf_slot_forget() leaves */
static bool version_state_empty(const remote_version_t *v) {
    return !v->remote_id[0] && !v->skin && !v->caps && !v->config_version && strcmp(v->sw_version, "unknown") == 0;
}

/* The remote's own settings (remote_slot<N>_config.json, sent back to it with every config
 * push): read whole, NULL when the file is missing or unreadable */
static char *remote_config_read(int slot) {
    char path[160];
    snprintf(path, sizeof(path), "%s/remote_slot%d_config.json", g_rf_version_dir, slot);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *text = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        long sz = ftell(f);
        if (sz >= 0 && sz <= 16384 && fseek(f, 0, SEEK_SET) == 0 && (text = (char *)malloc((size_t)sz + 1))) {
            if (fread(text, 1, (size_t)sz, f) == (size_t)sz) {
                text[sz] = 0;
            } else {
                free(text);
                text = NULL;
            }
        }
    }
    fclose(f);
    return text;
}

/* Slot <slot> takes the settings file read from another slot (text NULL: it has none) */
static void remote_config_move_to(int slot, const char *text) {
    char path[160];
    snprintf(path, sizeof(path), "%s/remote_slot%d_config.json", g_rf_version_dir, slot);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    char *out = NULL;
    if (root) {
        cJSON_DeleteItemFromObject(root, "slot"); /* the file names its slot */
        cJSON_AddNumberToObject(root, "slot", slot);
        out = cJSON_PrintUnformatted(root);
        cJSON_Delete(root);
    }
    if (out && atomic_file_write_str(path, out) == 0) {
        free(out);
        return;
    }
    free(out);
    unlink(path); /* no settings for this remote (or not readable): the stored ones are another's */
}

int rf_slot_map_follow_file(void) {
    uint8_t pipes[MAX_PAIRED_REMOTES];
    rf_slot_table_read(g_rf_slot_map_path, pipes); /* no table: slots and pipes the same */
    uint8_t old_slot_of_pipe[MAX_PAIRED_REMOTES + 1];
    pthread_mutex_lock(&s_slot_map_lock);
    slot_map_ensure_locked();
    memcpy(old_slot_of_pipe, s_slot_of_pipe, sizeof(old_slot_of_pipe));
    slot_map_set_locked(pipes);
    pthread_mutex_unlock(&s_slot_map_lock);

    /* a restored table is a permutation (two remotes can swap slots): copy every slot's state
     * first, then give each slot the state of the slot its pipe had */
    remote_version_t versions[MAX_PAIRED_REMOTES];
    remote_battery_t batteries[MAX_PAIRED_REMOTES];
    memcpy(versions, g_remote_versions, sizeof(versions));
    memcpy(batteries, g_remote_batteries, sizeof(batteries));
    char *configs[MAX_PAIRED_REMOTES] = {0};
    for (int s = 1; s <= MAX_PAIRED_REMOTES; s++) {
        uint8_t from = old_slot_of_pipe[pipes[s - 1]];
        if (from != s && from >= 1 && from <= MAX_PAIRED_REMOTES) configs[from - 1] = remote_config_read(from);
    }
    int changed = 0;
    for (int s = 1; s <= MAX_PAIRED_REMOTES; s++) {
        uint8_t from = old_slot_of_pipe[pipes[s - 1]];
        if (from == s || from < 1 || from > MAX_PAIRED_REMOTES) continue;
        changed++;
        g_remote_versions[s - 1] = versions[from - 1];
        g_remote_batteries[s - 1] = batteries[from - 1];
        char path[160];
        if (version_state_empty(&g_remote_versions[s - 1])) {
            snprintf(path, sizeof(path), "%s/remote_slot%d_version.json", g_rf_version_dir, s);
            unlink(path);
        } else {
            rf_state_save_version_file(s);
        }
        if (g_remote_batteries[s - 1].last_update_ms) {
            battery_cache_write(s);
        } else {
            battery_cache_path(s, path, sizeof(path));
            unlink(path);
        }
        remote_config_move_to(s, version_state_empty(&g_remote_versions[s - 1]) ? NULL : configs[from - 1]);
        printf("[*] Slot %d (CC2544 pipe %d) takes the remote state of slot %d\n", s, pipes[s - 1], from);
    }
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) free(configs[i]);
    return changed;
}

/* [pipe]: the last pipe-table reply showed a remote; and the same when the window opened */
static bool s_pipe_has_remote[MAX_PAIRED_REMOTES + 1];
static bool s_pipe_had_remote[MAX_PAIRED_REMOTES + 1];

void rf_slot_map_note_table(uint8_t pipe, bool has_remote) {
    if (pipe < 1 || pipe > MAX_PAIRED_REMOTES) return;
    pthread_mutex_lock(&s_slot_map_lock);
    s_pipe_has_remote[pipe] = has_remote;
    pthread_mutex_unlock(&s_slot_map_lock);
}

void rf_slot_map_set_pair_target(uint8_t slot, bool new_window) {
    pthread_mutex_lock(&s_slot_map_lock);
    s_pair_target = (slot >= 1 && slot <= MAX_PAIRED_REMOTES) ? slot : 0;
    s_pair_target_until_ms = get_monotonic_ms() + 45000; /* the 30 s window and the table reply */
    if (new_window) {
        /* a pipe whose slot holds a remote counts too: the choice must not depend on a complete
         * table read (a missed reply made an existing remote look new) */
        slot_map_ensure_locked();
        for (int p = 1; p <= MAX_PAIRED_REMOTES; p++)
            s_pipe_had_remote[p] = s_pipe_has_remote[p] || g_paired_remotes[s_slot_of_pipe[p] - 1].active;
    }
    pthread_mutex_unlock(&s_slot_map_lock);
}

void rf_slot_map_note_pair_success(void) {
    pthread_mutex_lock(&s_slot_map_lock);
    if (s_pair_target) {
        s_pair_target_until_ms = get_monotonic_ms() + 15000;
    }
    pthread_mutex_unlock(&s_slot_map_lock);
}

uint8_t rf_slot_map_claim_pipe(uint8_t pipe, bool from_pipe_table) {
    if (pipe < 1 || pipe > MAX_PAIRED_REMOTES) return pipe;
    char text[4 * MAX_PAIRED_REMOTES + 2] = "";
    uint8_t moved_to = 0;
    pthread_mutex_lock(&s_slot_map_lock);
    slot_map_ensure_locked();
    uint8_t slot = s_slot_of_pipe[pipe];
    uint8_t old_slot = slot;
    uint8_t target = s_pair_target;
    if (target && get_monotonic_ms() > s_pair_target_until_ms) target = s_pair_target = 0;
    /* Only the pipe table decides, not the order of the radio events: the remote that paired is
     * the pipe that holds a remote now and held none when the window opened. A frame (which can
     * come before the 4A) or a lock closing without a pairing moves nothing. */
    if (target && from_pipe_table && !s_pipe_had_remote[pipe]) {
        s_pair_target = 0;
        s_pipe_had_remote[pipe] = true;
        if (target != slot && !g_paired_remotes[target - 1].active) {
            /* the pipe goes to the chosen slot; the chosen slot's pipe (no remote) to the old one */
            uint8_t other = s_pipe_of_slot[target];
            s_pipe_of_slot[target] = pipe;
            s_slot_of_pipe[pipe] = target;
            s_pipe_of_slot[slot] = other;
            s_slot_of_pipe[other] = slot;
            /* the old slot held no remote (its pipe was empty): what the new remote's first frames
             * put there (version, caps, unit id, battery, link counters) moves with it below */
            size_t n = 0;
            for (int s = 1; s <= MAX_PAIRED_REMOTES; s++)
                n += (size_t)snprintf(text + n, sizeof(text) - n, s > 1 ? " %d" : "%d", s_pipe_of_slot[s]);
            snprintf(text + n, sizeof(text) - n, "\n");
            moved_to = target;
            slot = target;
        }
    }
    pthread_mutex_unlock(&s_slot_map_lock);
    /* the flash write after the unlock: every frame in and out takes this lock, and a waiter
     * spins (no futex on the Hub) */
    if (moved_to) {
        rf_slot_move_state(old_slot, moved_to);
        if (atomic_file_write_str(g_rf_slot_map_path, text) != 0)
            printf("[-] Slot table not saved to %s: slot %d holds pipe %d until hub_rf restarts\n",
                   g_rf_slot_map_path, moved_to, pipe);
        printf("[+] Paired remote on CC2544 pipe %d goes to the chosen slot %d\n", pipe, moved_to);
    }
    return slot;
}

/* The new remote's first frames were handled under the old slot of its pipe (it sends a version
 * report, its unit id and a sync request right after pairing, before the table reply that moves
 * the pipe): its state moves to the new slot, the old one is forgotten, and the config push it
 * asked for is started again for the new slot. */
void rf_slot_move_state(uint8_t from, uint8_t to) {
    if (from < 1 || from > MAX_PAIRED_REMOTES || to < 1 || to > MAX_PAIRED_REMOTES || from == to) return;
    paired_remote_t p = g_paired_remotes[from - 1];
    p.slot = to;
    p.active = true;
    g_paired_remotes[to - 1] = p;
    g_remote_versions[to - 1] = g_remote_versions[from - 1];
    g_remote_batteries[to - 1] = g_remote_batteries[from - 1];
    g_remote_link[to - 1] = g_remote_link[from - 1];
    rf_slot_forget(from);
    memset(&g_remote_link[from - 1], 0, sizeof(g_remote_link[from - 1]));
    rf_state_save_version_file(to);
    if (g_rf_push_starter) g_rf_push_starter();
}

void (*g_rf_push_starter)(void) = NULL;
volatile bool g_rf_push_deferred = false;

bool rf_any_slot_active(void) {
    for (int i = 0; i < MAX_PAIRED_REMOTES; i++) {
        if (g_paired_remotes[i].active) return true;
    }
    return false;
}

int rf_sync_pending_slot(char *diff, size_t diff_len) {
    int slot = 0;
    FILE *pdf = fopen("/data/harmony/rf_sync_pending_details.json", "r");
    if (!pdf) return 0;
    char dline[512] = {0};
    if (fread(dline, 1, sizeof(dline) - 1, pdf) > 0) {
        cJSON *pj = cJSON_Parse(dline);
        if (pj) {
            cJSON *ps = cJSON_GetObjectItem(pj, "slot");
            cJSON *pd = cJSON_GetObjectItem(pj, "diff");
            if (cJSON_IsNumber(ps)) slot = ps->valueint;
            if (diff && diff_len && cJSON_IsString(pd)) snprintf(diff, diff_len, "%s", pd->valuestring);
            cJSON_Delete(pj);
        }
    }
    fclose(pdf);
    return slot;
}

bool is_companion_slot(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return false;
    int idx = slot - 1;
    paired_remote_t *r = &g_paired_remotes[idx];
    if (r->model == REMOTE_MODEL_COMPANION || r->model == REMOTE_MODEL_SMART_CONTROL) return true;
    if (r->model == REMOTE_MODEL_JUNIPER || r->equad_id == RF_EQUAD_JUNIPER) return false;
    if (r->equad_id == RF_EQUAD_SMART_CONTROL) {
        r->model = REMOTE_MODEL_SMART_CONTROL;
        return true;
    }
    if (r->equad_id == RF_EQUAD_SPRITE_A || r->equad_id == RF_EQUAD_SPRITE_B) {
        r->model = REMOTE_MODEL_COMPANION;
        return true;
    }
    /* The CC2544 slot name, only while the EquadID is not known (a Juniper remote can carry
     * "Home" in its name: Harmony Ultimate Home) */
    if (r->equad_id == 0) {
        if (strstr(r->name, "20+") != NULL || strstr(r->name, "Smart") != NULL) {
            r->model = REMOTE_MODEL_SMART_CONTROL;
            return true;
        }
        if (strstr(r->name, "Companion") != NULL) {
            r->model = REMOTE_MODEL_COMPANION;
            return true;
        }
    }
    return false;
}

const char *get_remote_model_name(remote_model_t model) {
    switch (model) {
        case REMOTE_MODEL_SMART_CONTROL: return "smart_control";
        case REMOTE_MODEL_COMPANION: return "companion";
        case REMOTE_MODEL_JUNIPER: return "juniper";
        default: return "unknown";
    }
}

/* Family from the EquadID when it names one, else the family recorded from traffic */
static remote_model_t family_of(remote_model_t family, uint16_t equad_id) {
    if (equad_id == RF_EQUAD_JUNIPER) return REMOTE_MODEL_JUNIPER;
    if (equad_id == RF_EQUAD_SMART_CONTROL) return REMOTE_MODEL_SMART_CONTROL;
    if (equad_id == RF_EQUAD_SPRITE_A || equad_id == RF_EQUAD_SPRITE_B) return REMOTE_MODEL_COMPANION;
    if (equad_id != 0) return REMOTE_MODEL_UNKNOWN; /* 0x4024, 0x880E or anything else */
    return family;
}

typedef struct {
    uint8_t skin;
    const char *id;
    const char *name;
} juniper_product_t;

static const juniper_product_t k_juniper[] = {
    { RF_SKIN_ULTIMATE,        "ultimate",      "Harmony Ultimate" },
    { RF_SKIN_ULTIMATE_ONE,    "ultimate_one",  "Harmony Ultimate One" },
    { RF_SKIN_ULTIMATE_HOME_A, "ultimate_home", "Harmony Ultimate Home" },
    { RF_SKIN_ULTIMATE_HOME_B, "ultimate_home", "Harmony Ultimate Home" },
    { RF_SKIN_ELITE,           "elite",         "Harmony Elite" },
    { RF_SKIN_950,             "950",           "Harmony 950" },
};

const char *rf_model_id_for(remote_model_t family, uint16_t equad_id, uint8_t skin) {
    switch (family_of(family, equad_id)) {
        case REMOTE_MODEL_SMART_CONTROL: return "smart_control";
        case REMOTE_MODEL_COMPANION: return "companion";
        case REMOTE_MODEL_JUNIPER:
            if (skin == 0) return "unknown"; /* never guess: unknown until the remote reports it */
            for (size_t i = 0; i < sizeof(k_juniper) / sizeof(k_juniper[0]); i++) {
                if (k_juniper[i].skin == skin) return k_juniper[i].id;
            }
            return "touch";
        default: return "unknown";
    }
}

const char *rf_model_name_for(remote_model_t family, uint16_t equad_id, uint8_t skin) {
    switch (family_of(family, equad_id)) {
        case REMOTE_MODEL_SMART_CONTROL: return "Harmony Smart Control";
        case REMOTE_MODEL_COMPANION: return "Harmony Companion";
        case REMOTE_MODEL_JUNIPER:
            if (skin == 0) return NULL;
            for (size_t i = 0; i < sizeof(k_juniper) / sizeof(k_juniper[0]); i++) {
                if (k_juniper[i].skin == skin) return k_juniper[i].name;
            }
            return "Harmony Touch"; /* the stock remote UI's fallback */
        default: return NULL;
    }
}

const char *rf_slot_model_id(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return "unknown";
    is_companion_slot(slot); /* records a Sprite family from the EquadID or the slot name */
    const paired_remote_t *r = &g_paired_remotes[slot - 1];
    return rf_model_id_for(r->model, r->equad_id, g_remote_versions[slot - 1].skin);
}

const char *rf_slot_model_name(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return NULL;
    is_companion_slot(slot);
    const paired_remote_t *r = &g_paired_remotes[slot - 1];
    return rf_model_name_for(r->model, r->equad_id, g_remote_versions[slot - 1].skin);
}

void rf_note_slot_family(uint8_t slot, remote_model_t family) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    if (g_paired_remotes[slot - 1].model == REMOTE_MODEL_UNKNOWN) g_paired_remotes[slot - 1].model = family;
}

void rf_slot_saw_protocol_frame(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    bool was_heard = g_paired_remotes[slot - 1].heard;
    g_paired_remotes[slot - 1].heard = true;
    if (g_paired_remotes[slot - 1].equad_id == 0) rf_note_slot_family(slot, REMOTE_MODEL_JUNIPER);
    /* A push put off for this remote: it is awake now, so it can take it */
    if (!was_heard && g_rf_push_deferred && !g_config_push_running &&
        g_paired_remotes[slot - 1].active && !is_companion_slot(slot)) {
        g_rf_push_deferred = false;
        if (g_rf_push_starter) g_rf_push_starter();
    }
}

/* Key event counter, per slot. Key frames are sent once and never acknowledged; a remote that
 * numbers them (1..255, then 1 again) lets the Hub count the ones that did not arrive. */
static struct {
    uint8_t last;        /* counter of the last key frame, 0 = none since hub_rf started */
    bool restart_hint;   /* the remote may have restarted since that frame */
    uint16_t down_code;  /* key whose press or hold came last without its release, 0 = none */
} s_key_track[MAX_PAIRED_REMOTES];

void rf_key_note_restart_hint(uint8_t slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    s_key_track[slot - 1].restart_hint = true;
}

void rf_key_note_event(uint8_t slot, uint8_t counter, uint16_t key_code, uint8_t state, const char *name) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES || counter == 0) return;
    remote_link_stats_t *ls = &g_remote_link[slot - 1];
    const char *action = (state == 0x01) ? "press" : ((state == 0x02) ? "hold" : "release");
    if (!name) name = "";
    uint8_t last = s_key_track[slot - 1].last;
    bool hint = s_key_track[slot - 1].restart_hint;
    uint16_t down = s_key_track[slot - 1].down_code;

    ls->key_events++;
    unsigned missing = 0;
    if (last) {
        /* Numbers between the one expected and this one, over the ring 1..255 */
        unsigned gap = (unsigned)(counter + 255 - rf_key_counter_next(last)) % 255;
        if (counter == last && !(hint && counter == 1)) {
            /* The same number again is the same frame twice, not 254 lost ones */
            printf("[!] RF key event received twice: slot %u, #%u (%s %s)\n", slot, counter, action, name);
            return;
        }
        if (hint && counter - 1u <= gap) {
            /* A restarted remote counts from 1 again: frames 1..counter-1 are the missing ones
             * (none when this is number 1), and it no longer holds the key it had down */
            missing = counter - 1u;
            down = 0;
        } else {
            missing = gap;
        }
    }

    if (missing) {
        /* What the frames around the gap tell about the missing ones; each is counted once */
        unsigned left = missing;
        if (state == 0x01) {
            if (down) { ls->key_lost_release++; left--; }       /* the key before never got its release */
        } else if (down != key_code) {
            ls->key_lost_press++; left--;                       /* this key's press was not seen */
            if (down && left) { ls->key_lost_release++; left--; down = 0; } /* nor the release of the key before */
        }
        ls->key_lost_other += left;
        ls->key_events_lost += missing;
        printf("[!] RF key events lost: slot %u, %u missing before #%u (%s %s)\n", slot, missing, counter, action, name);
    }

    if (state != 0x00) down = key_code;
    else if (down == key_code) down = 0;
    s_key_track[slot - 1].last = counter;
    s_key_track[slot - 1].restart_hint = false;
    s_key_track[slot - 1].down_code = down;
}

int rf_slot_apply_unit_id(uint8_t slot, const uint8_t *payload) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES || !payload) return 0;
    int idx = slot - 1;
    size_t n = payload[0];
    if (n > RF_UNIT_ID_MAX) n = RF_UNIT_ID_MAX;
    char id[RF_UNIT_ID_MAX + 1] = {0};
    size_t k = 0;
    for (size_t i = 0; i < n; i++) {
        uint8_t c = payload[1 + i];
        /* letters, digits and - _ . only (a factory id is digits and a dash): the id goes into
           JSON built with "%s" (the version file, the RF status object), where a quote or a
           backslash made the whole object invalid */
        if (!isalnum(c) && c != '-' && c != '_' && c != '.') break;
        id[k++] = (char)c;
    }
    id[k] = '\0';
    /* the skin byte: 0 = not reported (an older remote pads it with zeros); only read when the
     * id leaves room for it */
    uint8_t skin = (n < RF_UNIT_ID_SKIN_OFF) ? payload[RF_UNIT_ID_SKIN_OFF] : 0;
    int changed = 0;
    if (k > 0 && strcmp(g_remote_versions[idx].remote_id, id) != 0) {
        snprintf(g_remote_versions[idx].remote_id, sizeof(g_remote_versions[idx].remote_id), "%s", id);
        printf("[*] UNIT_ID: Slot %d -> %s\n", slot, id);
        changed = 1;
    }
    if (skin && g_remote_versions[idx].skin != skin) {
        g_remote_versions[idx].skin = skin;
        {
            const char *mn = rf_slot_model_name(slot);
            printf("[*] SKIN: Slot %d -> 0x%02X (%s)\n", slot, skin, mn ? mn : "unknown product");
        }
        changed = 1;
    }
    return changed;
}

/* write() until everything is out (a socket may take a large buffer in pieces) */
static int write_all(int fd, const void *data, size_t len) {
    const char *p = (const char *)data;
    while (len > 0) {
        ssize_t w = write(fd, p, len);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        p += w;
        len -= (size_t)w;
    }
    return 0;
}

int http_request_local(int port, const char *method, const char *endpoint, const char *json_body,
                       char *resp, size_t resp_len, int timeout_ms) {
    if (resp && resp_len) resp[0] = '\0';
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;

    /* Non-blocking connect with 150ms timeout */
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in sin;
    memset(&sin, 0, sizeof(sin));
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(sock, (struct sockaddr *)&sin, sizeof(sin));
    if (rc < 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval ctv = { .tv_sec = 0, .tv_usec = 150000 };
        if (select(sock + 1, NULL, &wfds, NULL, &ctv) <= 0) {
            close(sock);
            return -1;
        }
        int err = 0;
        socklen_t elen = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (err) { close(sock); return -1; }
    } else if (rc < 0) {
        close(sock);
        return -1;
    }

    /* Restore blocking mode with timeouts */
    fcntl(sock, F_SETFL, flags);
    struct timeval tv = { .tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* Headers and body are written separately: the body (the RF status object is 1 to 3 KB)
     * used to go through a 1 KB buffer, which truncated it while write() was given the
     * untruncated length, so the daemon got a cut body plus stack bytes and answered 400 */
    char hdr[256];
    size_t blen = json_body ? strlen(json_body) : 0;
    int hdr_len = snprintf(hdr, sizeof(hdr),
        "%s %s HTTP/1.1\r\n"
        "Host: 127.0.0.1:%d\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n\r\n",
        method, endpoint, port, blen);
    if (hdr_len <= 0 || (size_t)hdr_len >= sizeof(hdr) ||
        write_all(sock, hdr, (size_t)hdr_len) != 0 ||
        (blen && write_all(sock, json_body, blen) != 0)) {
        close(sock);
        return -1;
    }
    shutdown(sock, SHUT_WR);

    /* Keep the headers and as much of the body as fits; drain the rest */
    char buf[4096], drain[512];
    size_t got = 0;
    for (;;) {
        ssize_t r = got < sizeof(buf) - 1 ? read(sock, buf + got, sizeof(buf) - 1 - got) : read(sock, drain, sizeof(drain));
        if (r <= 0) break;
        if (got < sizeof(buf) - 1) got += (size_t)r;
    }
    close(sock);
    buf[got] = '\0';

    int status = -1;
    if (sscanf(buf, "HTTP/%*s %d", &status) != 1) return -1;
    const char *body = strstr(buf, "\r\n\r\n");
    if (resp && resp_len && body) snprintf(resp, resp_len, "%s", body + 4);
    return status;
}

int http_post_local(int port, const char *endpoint, const char *json_body) {
    return http_request_local(port, "POST", endpoint, json_body, NULL, 0, 400);
}

int rf_state_version_json(int slot, char *buf, size_t len) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES || !buf) return -1;
    const remote_version_t *v = &g_remote_versions[slot - 1];
    int n;
    char extra[80] = "";
    if (v->remote_id[0]) snprintf(extra, sizeof(extra), ",\"remote_id\":\"%s\"", v->remote_id);
    if (v->skin) {
        size_t el = strlen(extra);
        snprintf(extra + el, sizeof(extra) - el, ",\"skin\":%u", (unsigned)v->skin);
    }
    n = snprintf(buf, len, "{\"slot\":%d,\"version\":\"%s\",\"config_version\":%d,\"caps\":%d%s,\"timestamp\":%ld}\n",
                 slot, v->sw_version, v->config_version, v->caps, extra, (long)time(NULL));
    return (n > 0 && (size_t)n < len) ? n : -1;
}

void rf_state_save_version_file(int slot) {
    char buf[256];
    if (rf_state_version_json(slot, buf, sizeof(buf)) < 0) return;
    /* synced temporary file renamed over the old one (atomic_file.h): a power cut or a full
     * flash during the write must not leave an empty or cut file */
    char vpath[160];
    snprintf(vpath, sizeof(vpath), "%s/remote_slot%d_version.json", g_rf_version_dir, slot);
    if (atomic_file_write_str(vpath, buf) != 0) {
        fprintf(stderr, "[-] Could not save %s\n", vpath);
    }
}

/* A config push delivered: the remote applies it on receipt but reports its version only on link
 * and every 24 h, so record the new config version here. Written with the same writer as the
 * reports, so the unit id and skin stay in the file (a hub_rf restart reads the model back). */
void rf_state_note_config_pushed(int slot, int config_ver) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    g_remote_versions[slot - 1].config_version = config_ver;
    rf_state_save_version_file(slot);
}

void rf_state_load_version_file(int slot) {
    if (slot < 1 || slot > MAX_PAIRED_REMOTES) return;
    char vpath[64];
    snprintf(vpath, sizeof(vpath), "%s/remote_slot%d_version.json", g_rf_version_dir, slot);
    FILE *fv = fopen(vpath, "r");
    if (!fv) return;
    char vbuf[256] = {0};
    if (fread(vbuf, 1, sizeof(vbuf) - 1, fv) > 0) {
        cJSON *vroot = cJSON_Parse(vbuf);
        if (vroot) {
            remote_version_t *v = &g_remote_versions[slot - 1];
            cJSON *jv = cJSON_GetObjectItem(vroot, "version");
            cJSON *jcv = cJSON_GetObjectItem(vroot, "config_version");
            cJSON *jcaps = cJSON_GetObjectItem(vroot, "caps");
            cJSON *jid = cJSON_GetObjectItem(vroot, "remote_id");
            cJSON *jskin = cJSON_GetObjectItem(vroot, "skin");
            if (cJSON_IsNumber(jskin) && jskin->valueint > 0 && jskin->valueint < 256) v->skin = (uint8_t)jskin->valueint;
            if (cJSON_IsNumber(jcaps)) v->caps = (uint8_t)jcaps->valueint;
            if (jv && cJSON_IsString(jv) && jv->valuestring) snprintf(v->sw_version, sizeof(v->sw_version), "%s", jv->valuestring);
            if (jcv && cJSON_IsNumber(jcv)) v->config_version = (uint8_t)jcv->valueint;
            if (jid && cJSON_IsString(jid) && jid->valuestring) snprintf(v->remote_id, sizeof(v->remote_id), "%s", jid->valuestring);
            cJSON_Delete(vroot);
        }
    }
    fclose(fv);
}
