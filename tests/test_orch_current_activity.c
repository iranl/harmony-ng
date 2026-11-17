/*
 * orch_get_current_activity() hands out a copy, not the orchestrator's own buffer.
 *
 * HTTP workers, the WebSocket and the MQTT command channel read the current activity while the
 * main loop switches it. The getter used to return the orchestrator's buffer after unlocking it,
 * so a switch could rewrite the string while a caller was still copying it (a GET during a switch
 * from "-1" to "12" could read "11"). The test includes orchestrator.c to reach that buffer and
 * switches the activity the way the orchestrator does (under g_act_lock).
 */
#include "../payload/source/orchestrator.c"

#include <assert.h>

/* Stubs for what orchestrator.c calls; none of them runs in this test */
int g_dry_run = 0;
const cJSON *config_store_activities_acquire(cfg_snap_t **snap) { *snap = NULL; return NULL; }
void config_store_release(cfg_snap_t **snap) { *snap = NULL; }
void hub_events_emit(hub_event_type_t type, cJSON *payload) { (void)type; cJSON_Delete(payload); }
int hub_events_subscribe(hub_event_sink_fn fn, void *user) { (void)fn; (void)user; return 0; }
int hw_device_bt_disconnect(const char *device_id) { (void)device_id; return 0; }
int hw_device_bt_host(const char *device_id, char *out, size_t outlen) { (void)device_id; (void)out; (void)outlen; return -1; }
int hw_device_command_send(const char *device_id, const char *command) { (void)device_id; (void)command; return 0; }
int hw_ir_cancel(uint8_t ports) { (void)ports; return 0; }
int hw_ir_send_harmony_keycode(const char *keycode, uint8_t ports, uint8_t repeats) { (void)keycode; (void)ports; (void)repeats; return 0; }
int hw_ir_send_pronto(const char *pronto_hex, uint8_t ports, uint8_t repeats) { (void)pronto_hex; (void)ports; (void)repeats; return 0; }
int orch_build_steps(const char *current_id, const char *target_id,
                     orch_step_t *main_steps, size_t max_main, size_t *main_count_out,
                     orch_bg_step_t *bg_steps, size_t max_bg, size_t *bg_count_out,
                     uint32_t *switched_at_ms, uint32_t *true_end_ms) {
    (void)current_id; (void)target_id; (void)main_steps; (void)max_main; (void)main_count_out;
    (void)bg_steps; (void)max_bg; (void)bg_count_out; (void)switched_at_ms; (void)true_end_ms;
    return -1;
}
int run_bt_saved_script(const char *type, const char *bdaddr, const char *script,
                        int gap_ms, char *out, size_t outlen) {
    (void)type; (void)bdaddr; (void)script; (void)gap_ms; (void)out; (void)outlen;
    return -1;
}

static int g_fails = 0;

static void check(int cond, const char *what) {
    printf("  %s %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) g_fails++;
}

/* What the orchestrator does when a switch completes */
static void switch_to(const char *id) {
    pthread_mutex_lock(&g_act_lock);
    strncpy(g_current_act, id, sizeof(g_current_act) - 1);
    g_current_act[sizeof(g_current_act) - 1] = '\0';
    pthread_mutex_unlock(&g_act_lock);
}

static const char *g_other_copy = NULL;

/* Reader for the stress check: copies what the getter returns, the way the HTTP handlers do,
 * while main() switches between "-1" and "12". A copy made without the lock can catch the
 * buffer halfway through a switch: "11" ("1" of "12" over the "-" of "-1"). */
static volatile int g_stress_stop = 0;
static long g_stress_reads = 0, g_stress_torn = 0;

static void *stress_reader(void *arg) {
    (void)arg;
    while (!g_stress_stop) {
        char copy[32];
        snprintf(copy, sizeof(copy), "%s", orch_get_current_activity());
        if (strcmp(copy, "-1") != 0 && strcmp(copy, "12") != 0) g_stress_torn++;
        g_stress_reads++;
    }
    return NULL;
}

static void *other_thread(void *arg) {
    (void)arg;
    g_other_copy = orch_get_current_activity();
    return NULL;
}

int main(void) {
    printf("[-] orch_get_current_activity() returns a copy\n");
    switch_to("-1");
    const char *got = orch_get_current_activity();
    check(strcmp(got, "-1") == 0, "the copy holds the current activity");
    check(got != g_current_act, "the copy is not the orchestrator's own buffer");

    switch_to("12");
    check(strcmp(got, "-1") == 0, "a switch does not rewrite a copy already handed out");
    check(strcmp(orch_get_current_activity(), "12") == 0, "the next call returns the new activity");

    pthread_t t;
    assert(pthread_create(&t, NULL, other_thread, NULL) == 0);
    pthread_join(t, NULL);
    const char *mine = orch_get_current_activity();
    check(g_other_copy != NULL && g_other_copy != mine, "each thread gets its own copy");

    pthread_t r;
    assert(pthread_create(&r, NULL, stress_reader, NULL) == 0);
    for (int i = 0; i < 1000000; i++) switch_to((i & 1) ? "12" : "-1");
    g_stress_stop = 1;
    pthread_join(r, NULL);
    printf("    %ld reads during 1000000 switches, %ld not a whole id\n", g_stress_reads, g_stress_torn);
    check(g_stress_reads > 0 && g_stress_torn == 0, "a read during switches always gets a whole id");

    if (g_fails) {
        printf("[!] %d check(s) failed\n", g_fails);
        return 1;
    }
    printf("[+] Current activity copy: all checks passed\n");
    return 0;
}
