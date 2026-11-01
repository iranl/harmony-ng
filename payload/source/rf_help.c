/* Help (ADR-0004) over RF. The remote asks for Help; the Hub daemon re-sends the activity's
 * commands except power toggles (POST /api/activity/help). Once they are sent, the remote
 * gets one RF_MSG_HELP_STATUS question per device with only a power toggle, and its answers
 * go to POST /api/activity/help/answer. */
#define _GNU_SOURCE
#include "rf_help.h"
#include "rf_spi.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <pthread.h>

#define HELP_MAX_WAIT_MS 120000  /* longest activity start sequence Help waits for */

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_running;

typedef struct {
    uint8_t slot;
    char act_id[13];
} help_job_t;

static help_job_t s_job;

static uint8_t frame_slot(const rf_frame_t *f) {
    uint8_t slot = f->dev_slot & 0x0F;
    return (slot < 1 || slot > 2) ? 1 : slot;
}

static void copy_id(char *out, size_t len, const uint8_t *src, size_t n) {
    size_t j = 0;
    for (size_t i = 0; i < n && src[i] && j + 1 < len; i++) {
        if (isalnum(src[i]) || src[i] == '-') out[j++] = (char)src[i];
    }
    out[j] = '\0';
}

static int send_status(uint8_t slot, const uint8_t *payload, bool acked) {
    rf_frame_t f;
    rf_build_frame(&f, slot, RF_MSG_HELP_STATUS, ++g_tx_seq, acked ? RF_FLAG_ACK_REQ : 0, payload, 25);
    if (!acked) return rf_send_frame(&f) < 0 ? -1 : 0;
    for (int attempt = 0; attempt < 3; attempt++) {
        if (rf_send_frame_acked(&f, 300) == 0) return 0;
    }
    printf("[-] Help: no ACK for status 0x%02X from slot %d\n", payload[0], slot);
    return -1;
}

static int send_code(uint8_t slot, uint8_t code, bool acked) {
    uint8_t p[25] = {0};
    p[0] = code;
    return send_status(slot, p, acked);
}

/* Waits until the daemon has sent the re-sent commands, so questions never overlap them */
static void wait_until_idle(void) {
    for (int waited = 0; waited < HELP_MAX_WAIT_MS; waited += 500) {
        char resp[256];
        if (http_request_local(DAEMON_PORT, "GET", "/api/activity", NULL, resp, sizeof(resp), 1000) == 200) {
            cJSON *j = cJSON_Parse(resp);
            bool busy = j && (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "busy")) ||
                              cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "sending")));
            cJSON_Delete(j);
            if (!busy) return;
        }
        usleep(500000);
    }
    printf("[-] Help: activity still busy after %d s, asking anyway\n", HELP_MAX_WAIT_MS / 1000);
}

static void *help_worker(void *arg) {
    help_job_t job = *(help_job_t *)arg;
    char body[64], resp[2048];
    snprintf(body, sizeof(body), "{\"activity\":\"%s\"}", job.act_id);
    int st = http_request_local(DAEMON_PORT, "POST", "/api/activity/help", body, resp, sizeof(resp), 3000);
    printf("[*] Help for activity %s from slot %d: HTTP %d\n", job.act_id, job.slot, st);

    cJSON *root = st == 200 ? cJSON_Parse(resp) : NULL;
    cJSON *devs = root ? cJSON_GetObjectItemCaseSensitive(root, "toggleDevices") : NULL;
    if (!cJSON_IsArray(devs)) {
        send_code(job.slot, st == 409 && strstr(resp, "busy") ? RF_HELP_BUSY : RF_HELP_FAILED, true);
    } else if (send_code(job.slot, RF_HELP_STARTED, true) == 0) {
        wait_until_idle();
        int n = cJSON_GetArraySize(devs);
        if (n > 255) n = 255;
        if (n == 0) send_code(job.slot, RF_HELP_NO_QUESTIONS, true);
        for (int i = 0; i < n; i++) {
            cJSON *d = cJSON_GetArrayItem(devs, i);
            cJSON *jid = cJSON_GetObjectItemCaseSensitive(d, "id");
            cJSON *jname = cJSON_GetObjectItemCaseSensitive(d, "name");
            uint8_t p[25] = {0};
            p[0] = RF_HELP_QUESTION;
            p[1] = (uint8_t)i;
            p[2] = (uint8_t)n;
            if (cJSON_IsString(jid)) strncpy((char *)&p[3], jid->valuestring, 8);
            if (cJSON_IsString(jname)) strncpy((char *)&p[11], jname->valuestring, 14);
            if (send_status(job.slot, p, true) != 0) break;
        }
    }
    cJSON_Delete(root);

    pthread_mutex_lock(&s_lock);
    s_running = false;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

void rf_help_request(const rf_frame_t *f) {
    uint8_t slot = frame_slot(f);
    pthread_mutex_lock(&s_lock);
    if (s_running) {
        /* A retry because the remote missed RF_HELP_STARTED, or a second press */
        pthread_mutex_unlock(&s_lock);
        send_code(slot, RF_HELP_STARTED, false);
        return;
    }
    s_job.slot = slot;
    copy_id(s_job.act_id, sizeof(s_job.act_id), f->payload, 12);
    pthread_t th;
    s_running = pthread_create(&th, NULL, help_worker, &s_job) == 0;
    if (s_running) pthread_detach(th);
    pthread_mutex_unlock(&s_lock);
    if (!s_running) send_code(slot, RF_HELP_FAILED, false);
}

/* payload [0] 1 = on, 0 = off, [1..8] device id */
void rf_help_answer(const rf_frame_t *f) {
    uint8_t slot = frame_slot(f);
    if (f->flags & RF_FLAG_ACK_REQ) {
        rf_frame_t ack;
        rf_build_ack(&ack, slot, f->seq, 0x00);
        rf_send_frame(&ack);
    }
    char dev_id[9];
    copy_id(dev_id, sizeof(dev_id), &f->payload[1], 8);
    char body[64];
    snprintf(body, sizeof(body), "{\"device\":\"%s\",\"on\":%s}", dev_id, f->payload[0] ? "true" : "false");
    printf("[*] Help answer from slot %d: device %s is %s\n", slot, dev_id, f->payload[0] ? "on" : "off");
    http_post_local(DAEMON_PORT, "/api/activity/help/answer", body);
}
