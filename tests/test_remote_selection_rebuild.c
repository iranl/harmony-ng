/*
 * test_remote_selection_rebuild.c - The home screen selection and a pending "press OK to confirm"
 * are indexes into the activity list. A config push rebuilds that list (build_activity_list())
 * with a new length; the old indexes were kept, so the carousel showed a deleted activity and OK
 * started an entry the Hub no longer has. They now follow the activity they pointed to, or are
 * clamped / dropped when it is gone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "remote/remote_state.h"
#include "remote/remote_config.h"

static int s_failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { printf("  [FAIL] " __VA_ARGS__); printf("\n"); s_failures++; } } while (0)

/* The pushed activity list: ids "10".."(10+n-1)", skipping the ids in skip */
static void push_activities(int first, int n, const char *skip) {
    g_rf_activity_count = 0;
    for (int i = 0; i < n; i++) {
        char id[13];
        snprintf(id, sizeof(id), "%d", first + i);
        if (skip && strstr(skip, id)) continue;
        rf_activity_t *a = &g_rf_activities[g_rf_activity_count++];
        memset(a, 0, sizeof(*a));
        snprintf(a->id, sizeof(a->id), "%s", id);
        snprintf(a->name, sizeof(a->name), "Activity %s", id);
    }
    build_activity_list();
}

int main(void) {
    /* Six activities, the user on the last card, a confirmation pending for the fifth */
    push_activities(10, 6, NULL);
    g_selected_activity = 5;
    g_confirm_pending_idx = 4;
    g_confirm_pending_deadline_ms = get_monotonic_ms() + 4000;

    /* The push leaves three: 10, 11, 12 */
    push_activities(10, 3, NULL);
    CHECK(g_activity_count == 3, "3 activities after the push (%d)", g_activity_count);
    CHECK(g_selected_activity >= 0 && g_selected_activity < g_activity_count,
          "selection %d is outside the 3 activities", g_selected_activity);
    CHECK(g_confirm_pending_idx == -1, "the confirmation for a deleted activity is still pending (index %d)", g_confirm_pending_idx);

    /* Six again, the user on "15" with a confirmation for "14"; "11" and "12" are deleted:
       both indexes move to the same activities */
    push_activities(10, 6, NULL);
    g_selected_activity = 5;
    g_confirm_pending_idx = 4;
    g_confirm_pending_deadline_ms = get_monotonic_ms() + 4000;
    push_activities(10, 6, "11 12");
    CHECK(g_activity_count == 4, "4 activities after the push (%d)", g_activity_count);
    CHECK(g_selected_activity >= 0 && g_selected_activity < g_activity_count &&
          strcmp(g_activities[g_selected_activity].id, "15") == 0,
          "the selection should stay on activity 15 (index %d)", g_selected_activity);
    CHECK(g_confirm_pending_idx >= 0 && g_confirm_pending_idx < g_activity_count &&
          strcmp(g_activities[g_confirm_pending_idx].id, "14") == 0,
          "the pending confirmation should stay on activity 14 (index %d)", g_confirm_pending_idx);

    if (s_failures) {
        printf("[-] test_remote_selection_rebuild: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("[+] test_remote_selection_rebuild: selection and confirmation follow their activity across a config push\n");
    return 0;
}
