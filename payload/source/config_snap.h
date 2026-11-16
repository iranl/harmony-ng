/*
 * Shared read-only snapshots of a cached configuration tree (devices.json, activities.json).
 *
 * A snapshot is a published tree with a count of the readers holding it. A save never changes a
 * published tree: it publishes a new snapshot, and the old one is freed when its last reader
 * releases it (at once when nobody holds it). Readers pay one counter update instead of a deep
 * copy of the whole list.
 *
 * Every function here is called with the config store lock held (config_store.c); readers use
 * config_store_*_acquire() / config_store_release() from config_store.h.
 */
#ifndef HARMONY_CONFIG_SNAP_H
#define HARMONY_CONFIG_SNAP_H

#include <stdlib.h>
#include "cJSON.h"

struct cfg_snap_slot;

struct cfg_snap {
    cJSON *tree;                /* never changed once published */
    int refs;                   /* readers holding this version */
    struct cfg_snap_slot *slot; /* the list it is a version of */
};

struct cfg_snap_slot {
    struct cfg_snap *current; /* NULL before the first load and after an invalidation */
    int held;                 /* references not released yet, over all versions */
};

/* Frees a version that is neither current nor held by a reader */
static inline void cfg_snap_free_if_unused(struct cfg_snap *s) {
    if (!s || s->refs > 0 || s->slot->current == s) return;
    cJSON_Delete(s->tree);
    free(s);
}

/* Makes tree the current version (the slot owns it from now on); the previous version is freed
 * once nobody holds it. Returns -1 when out of memory: then the tree is not taken. */
static inline int cfg_snap_publish(struct cfg_snap_slot *slot, cJSON *tree) {
    struct cfg_snap *s = (struct cfg_snap *)calloc(1, sizeof(*s));
    if (!s) return -1;
    s->tree = tree;
    s->slot = slot;
    struct cfg_snap *old = slot->current;
    slot->current = s;
    cfg_snap_free_if_unused(old);
    return 0;
}

/* No current version any more (the next reader loads the file again) */
static inline void cfg_snap_unpublish(struct cfg_snap_slot *slot) {
    struct cfg_snap *old = slot->current;
    slot->current = NULL;
    cfg_snap_free_if_unused(old);
}

/* The current version with one more reference, or NULL when there is none */
static inline struct cfg_snap *cfg_snap_acquire(struct cfg_snap_slot *slot) {
    struct cfg_snap *s = slot->current;
    if (s) {
        s->refs++;
        slot->held++;
    }
    return s;
}

/* Gives a reference back and clears the handle; NULL handles are ignored */
static inline void cfg_snap_release(struct cfg_snap **snap) {
    struct cfg_snap *s = snap ? *snap : NULL;
    if (!s) return;
    *snap = NULL;
    s->refs--;
    s->slot->held--;
    cfg_snap_free_if_unused(s);
}

#endif /* HARMONY_CONFIG_SNAP_H */
