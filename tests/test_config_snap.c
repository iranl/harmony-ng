/*
 * Unit test of the shared configuration snapshots (payload/source/config_snap.h): a reader holds
 * a published tree while saves publish new ones. Checks that
 *  - a held snapshot survives a save and stays readable;
 *  - the old snapshot is freed at its last release, not before;
 *  - the current one is never freed by a release;
 *  - the reference counts go back to 0;
 *  - readers and writers in parallel threads (one mutex, as config_store.c) leave nothing behind.
 * Built with AddressSanitizer + LeakSanitizer by tests/run_mock_suite.sh:
 *   gcc -Wall -Wextra -O1 -g -fsanitize=address -pthread -Ipayload/source \
 *       tests/test_config_snap.c payload/source/cJSON.c -lm
 */
#include "config_snap.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } else printf("  PASS %s\n", msg); } while (0)

/* Live cJSON allocations, to see when a tree is freed */
static long g_live;
static pthread_mutex_t g_live_lock = PTHREAD_MUTEX_INITIALIZER;
static void *count_malloc(size_t n) {
    void *p = malloc(n);
    if (p) {
        pthread_mutex_lock(&g_live_lock);
        g_live++;
        pthread_mutex_unlock(&g_live_lock);
    }
    return p;
}
static void count_free(void *p) {
    if (p) {
        pthread_mutex_lock(&g_live_lock);
        g_live--;
        pthread_mutex_unlock(&g_live_lock);
    }
    free(p);
}
static long live(void) {
    pthread_mutex_lock(&g_live_lock);
    long n = g_live;
    pthread_mutex_unlock(&g_live_lock);
    return n;
}

static cJSON *make_tree(int version) {
    cJSON *t = cJSON_CreateObject();
    cJSON *devs = cJSON_AddArrayToObject(t, "devices");
    for (int i = 0; i < 20; i++) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "version", version);
        cJSON_AddStringToObject(d, "name", "Device");
        cJSON_AddItemToArray(devs, d);
    }
    return t;
}

/* Every device of a tree carries its version: reads the whole tree, -1 when mixed */
static int tree_version(const cJSON *t) {
    int v = -2;
    const cJSON *d = NULL;
    cJSON_ArrayForEach(d, cJSON_GetObjectItemCaseSensitive(t, "devices")) {
        int dv = cJSON_GetObjectItemCaseSensitive(d, "version")->valueint;
        if (v == -2) v = dv;
        else if (v != dv) return -1;
    }
    return v;
}

static struct cfg_snap_slot g_slot;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int g_stop;
static volatile int g_bad_reads;

static void *reader(void *arg) {
    (void)arg;
    while (!g_stop) {
        pthread_mutex_lock(&g_lock);
        struct cfg_snap *s = cfg_snap_acquire(&g_slot);
        pthread_mutex_unlock(&g_lock);
        if (!s) continue;
        if (tree_version(s->tree) < 0) g_bad_reads++; /* read without the lock */
        pthread_mutex_lock(&g_lock);
        cfg_snap_release(&s);
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

int main(void) {
    printf("[-] Shared configuration snapshots\n");
    cJSON_Hooks hooks = { count_malloc, count_free };
    cJSON_InitHooks(&hooks);

    long base = live();
    cJSON *v1 = make_tree(1);
    long per_tree = live() - base;
    CHECK(cfg_snap_publish(&g_slot, v1) == 0 && g_slot.current && g_slot.current->tree == v1, "first tree published");

    struct cfg_snap *held = cfg_snap_acquire(&g_slot);
    CHECK(held && held->tree == v1 && held->refs == 1 && g_slot.held == 1, "reader holds version 1");

    CHECK(cfg_snap_publish(&g_slot, make_tree(2)) == 0, "save publishes version 2");
    CHECK(g_slot.current != held && tree_version(g_slot.current->tree) == 2, "version 2 is current");
    CHECK(tree_version(held->tree) == 1, "held version 1 is still whole after the save");
    CHECK(live() - base == 2 * per_tree, "version 1 not freed while held");

    struct cfg_snap *held2 = cfg_snap_acquire(&g_slot);
    CHECK(held2 && tree_version(held2->tree) == 2 && g_slot.held == 2, "a second reader gets version 2");

    cfg_snap_release(&held);
    CHECK(held == NULL, "release clears the handle");
    CHECK(live() - base == per_tree, "version 1 freed at its last release");
    cfg_snap_release(&held); /* a cleared handle is ignored */
    CHECK(g_slot.held == 1, "releasing a cleared handle changes nothing");

    cfg_snap_release(&held2);
    CHECK(g_slot.current && tree_version(g_slot.current->tree) == 2 && live() - base == per_tree,
          "the current version is not freed by its last release");
    CHECK(g_slot.held == 0 && g_slot.current->refs == 0, "counts back to 0");

    /* A save with nobody holding the old one frees it at once */
    CHECK(cfg_snap_publish(&g_slot, make_tree(3)) == 0 && live() - base == per_tree,
          "unheld old version freed by the save itself");

    /* An invalidation keeps a held tree until its release, and the next acquire gets nothing */
    struct cfg_snap *held3 = cfg_snap_acquire(&g_slot);
    cfg_snap_unpublish(&g_slot);
    CHECK(g_slot.current == NULL && cfg_snap_acquire(&g_slot) == NULL, "no current version after an invalidation");
    CHECK(tree_version(held3->tree) == 3, "held tree survives the invalidation");
    cfg_snap_release(&held3);
    CHECK(live() == base && g_slot.held == 0, "invalidated tree freed at its release");

    /* Parallel readers and a writer publishing new versions */
    enum { READERS = 6, SAVES = 3000 };
    cfg_snap_publish(&g_slot, make_tree(0));
    pthread_t th[READERS];
    for (int i = 0; i < READERS; i++) pthread_create(&th[i], NULL, reader, NULL);
    for (int v = 1; v <= SAVES; v++) {
        cJSON *t = make_tree(v); /* built outside the lock, as a writer builds its copy */
        pthread_mutex_lock(&g_lock);
        if (cfg_snap_publish(&g_slot, t) != 0) cJSON_Delete(t);
        if (v % 500 == 0) cfg_snap_unpublish(&g_slot);
        pthread_mutex_unlock(&g_lock);
    }
    g_stop = 1;
    for (int i = 0; i < READERS; i++) pthread_join(th[i], NULL);
    CHECK(g_bad_reads == 0, "parallel readers always saw whole trees");
    CHECK(g_slot.held == 0, "parallel readers released every reference");
    pthread_mutex_lock(&g_lock);
    cfg_snap_unpublish(&g_slot);
    pthread_mutex_unlock(&g_lock);
    CHECK(live() == base, "nothing left after the last version is unpublished");

    if (g_failures) {
        printf("[-] %d check(s) failed\n", g_failures);
        return 1;
    }
    printf("[+] config snapshots: all checks passed\n");
    return 0;
}
