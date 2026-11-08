#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <pthread.h>

#include "remote/remote_state.h"
#include "remote/remote_rf_ota.h"
#include "remote/remote_theme.h"
#include "rf_proto.h"
#include "sha256.h"

/* Mock variables needed by remote_rf_ota.c */
remote_battery_info_t g_battery_info = { 80, 1, 4100, "battery" };
uint8_t g_remote_slot = 1;
bool g_screen_on = true;
volatile bool g_screen_dirty = false;
volatile bool g_is_ota_updating = false;
uint64_t g_ota_start_ms = 0;
volatile uint64_t g_ota_last_packet_ms = 0; /* volatile in remote_state.h */
uint32_t g_ota_total_bytes = 0;
uint32_t g_ota_received_bytes = 0;
uint16_t g_ota_total_chunks = 0;
uint16_t g_ota_received_chunks = 0;
time_t g_last_activity_time = 0;
FILE *g_ota_file = NULL;
int g_rf_fd = -1;
pthread_mutex_t g_rf_write_mutex = PTHREAD_MUTEX_INITIALIZER; /* RF frame writes (remote_state.c) */
uint8_t g_rf_pm = RF_PM_ACTIVE;   /* CC2544 power mode, set during an OTA transfer */
/* Power source; only read after a finished install, which this test never reaches (no OTA_END) */
bool is_remote_docked(void) { return true; }
theme_t g_theme; /* the OTA overlay draws in the theme's colours */

/* Stubs for icon rx and activities referenced by handle_icon_frame in remote_rf_ota */
FILE *g_icon_rx_file = NULL;
uint32_t g_icon_rx_bytes = 0;
uint32_t g_icon_rx_tot_bytes = 0;
uint16_t g_icon_rx_chunks = 0;
uint8_t g_icon_rx_type = 0;
char g_icon_rx_id[32] = {0};
activity_item_t g_activities[MAX_ACTIVITIES];
rf_activity_t g_rf_activities[MAX_ACTIVITIES];
int g_activity_count = 0;
int g_rf_activity_count = 0;
void fb_invalidate_png_cache(void) {}

/* The test moves this clock forward: the remote saves its resume checkpoint at most every 500 ms
 * (remote_rf_ota.c), and the test sends its chunks within a millisecond */
static uint64_t s_clock_skew_ms = 0;
uint64_t get_monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L) + s_clock_skew_ms;
}

/* Stubs for UI & power functions called by remote_rf_ota */
void fb_set_blank(bool blank) { (void)blank; }
void fb_fill_rect(int x, int y, int w, int h, uint16_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void fb_draw_rect(int x, int y, int w, int h, uint16_t color) { (void)x; (void)y; (void)w; (void)h; (void)color; }
void fb_draw_text_aa(int x, int y, const char *text, uint16_t color, const void *font) { (void)x; (void)y; (void)text; (void)color; (void)font; }
const char *tr(remote_str_id_t id) { (void)id; return "text"; }
void read_remote_battery(void) {}
void render_screen(void) {}

/* Mock RF transmit capturing */
static rf_frame_t s_last_sent_ack;
int rf_send_frame(rf_frame_t *f) {
    if (f) memcpy(&s_last_sent_ack, f, sizeof(s_last_sent_ack));
    return 0;
}

int main(void) {
    printf("[*] Starting test_rf_ota_resume unit test...\n");

    /* Clean old test artifacts */
    unlink("/tmp/remote_ui.ota.gz");
    unlink("/tmp/remote_ui.ota.meta");

    /* 1. Generate synthetic payload of 2,200 bytes = 100 chunks of 22 bytes */
    const size_t test_size = 2200;
    const uint16_t total_chunks = (uint16_t)((test_size + 21) / 22);
    uint8_t synthetic_data[2200];
    for (size_t i = 0; i < test_size; i++) {
        synthetic_data[i] = (uint8_t)((i * 37 + 13) & 0xFF);
    }

    uint32_t file_crc = rf_crc32(synthetic_data, test_size);
    uint8_t expected_digest[32] = {0};
    sha256_hash(synthetic_data, test_size, expected_digest);

    /* 2. Step 1: RF_MSG_OTA_START (Clean Start) */
    rf_frame_t start_f;
    memset(&start_f, 0, sizeof(start_f));
    start_f.msg_type = RF_MSG_OTA_START;
    start_f.seq = 1;
    start_f.payload[0] = (uint8_t)(test_size & 0xFF);
    start_f.payload[1] = (uint8_t)((test_size >> 8) & 0xFF);
    start_f.payload[2] = 0;
    start_f.payload[3] = 0;
    start_f.payload[4] = (uint8_t)(total_chunks & 0xFF);
    start_f.payload[5] = (uint8_t)((total_chunks >> 8) & 0xFF);
    start_f.payload[6] = (uint8_t)(file_crc & 0xFF);
    start_f.payload[7] = (uint8_t)((file_crc >> 8) & 0xFF);
    start_f.payload[8] = (uint8_t)((file_crc >> 16) & 0xFF);
    start_f.payload[9] = (uint8_t)((file_crc >> 24) & 0xFF);

    bool res = handle_ota_frame(&start_f);
    assert(res == true);
    assert(g_is_ota_updating == true);
    assert(g_ota_received_chunks == 0);
    /* ACK payload[2..3] must report resume_chunk = 0 */
    uint16_t res_chunk = (uint16_t)s_last_sent_ack.payload[2] | ((uint16_t)s_last_sent_ack.payload[3] << 8);
    assert(res_chunk == 0);
    printf("[+] Step 1 passed: OTA_START clean start initialized (resume_chunk=0)\n");

    /* 3. Step 2: Stream chunks 0 .. 49. Only the first chunk's checkpoint is written at once; 600 ms
     *    later the last one writes the next (contiguous count 50) */
    for (uint16_t c = 0; c < 50; c++) {
        if (c == 49) s_clock_skew_ms += 600;
        rf_frame_t chunk_f;
        memset(&chunk_f, 0, sizeof(chunk_f));
        chunk_f.msg_type = RF_MSG_OTA_CHUNK;
        chunk_f.flags = (c % 4 == 0 || c == 49) ? RF_FLAG_ACK_REQ : 0;
        chunk_f.seq = (uint8_t)(c & 0x0F);
        chunk_f.payload[0] = (uint8_t)(c & 0xFF);
        chunk_f.payload[1] = (uint8_t)((c >> 8) & 0xFF);
        chunk_f.payload[2] = 22;
        memcpy(&chunk_f.payload[3], synthetic_data + (c * 22), 22);

        assert(handle_ota_frame(&chunk_f) == true);
    }
    assert(g_ota_received_chunks == 50);
    printf("[+] Step 2 passed: Received 50 chunks (g_ota_received_chunks=50)\n");

    /* 4. Step 3: Retransmit duplicate chunks 20 .. 25 (idempotency check) */
    for (uint16_t c = 20; c <= 25; c++) {
        rf_frame_t dup_f;
        memset(&dup_f, 0, sizeof(dup_f));
        dup_f.msg_type = RF_MSG_OTA_CHUNK;
        dup_f.flags = RF_FLAG_ACK_REQ;
        dup_f.seq = (uint8_t)(c & 0x0F);
        dup_f.payload[0] = (uint8_t)(c & 0xFF);
        dup_f.payload[1] = (uint8_t)((c >> 8) & 0xFF);
        dup_f.payload[2] = 22;
        memcpy(&dup_f.payload[3], synthetic_data + (c * 22), 22);

        assert(handle_ota_frame(&dup_f) == true);
    }
    /* Count must STILL be 50 without duplicate increments */
    assert(g_ota_received_chunks == 50);
    assert(g_ota_received_bytes == 50 * 22);
    printf("[+] Step 3 passed: Duplicate chunk retransmission handled idempotently (no count inflation)\n");

    /* 5. Step 4: Simulate RF link disruption & restart with Resume */
    if (g_ota_file) {
        fflush(g_ota_file);
        fclose(g_ota_file);
        g_ota_file = NULL;
    }
    g_is_ota_updating = false;

    /* New OTA_START with same CRC32 */
    res = handle_ota_frame(&start_f);
    assert(res == true);
    assert(g_is_ota_updating == true);
    res_chunk = (uint16_t)s_last_sent_ack.payload[2] | ((uint16_t)s_last_sent_ack.payload[3] << 8);
    printf("[*] Remote returned resume_chunk=%u in ACK\n", res_chunk);
    assert(res_chunk == 50);
    assert(g_ota_received_chunks == 50);
    assert(g_ota_received_bytes == 50 * 22);
    printf("[+] Step 4 passed: Resume detected checkpoint at chunk 50!\n");

    /* 6. Step 5: Send remaining chunks 50 .. 99 */
    for (uint16_t c = 50; c < 100; c++) {
        rf_frame_t chunk_f;
        memset(&chunk_f, 0, sizeof(chunk_f));
        chunk_f.msg_type = RF_MSG_OTA_CHUNK;
        chunk_f.flags = ((c + 1 == 100) || (c % 4 == 0)) ? RF_FLAG_ACK_REQ : 0;
        chunk_f.seq = (uint8_t)(c & 0x0F);
        chunk_f.payload[0] = (uint8_t)(c & 0xFF);
        chunk_f.payload[1] = (uint8_t)((c >> 8) & 0xFF);
        chunk_f.payload[2] = 22;
        memcpy(&chunk_f.payload[3], synthetic_data + (c * 22), 22);

        assert(handle_ota_frame(&chunk_f) == true);
    }
    assert(g_ota_received_chunks == 100);
    printf("[+] Step 5 passed: Streamed chunks 50..99 to completion (total=100)\n");

    /* 7. Step 6: Verify file on disk matches byte-for-byte */
    if (g_ota_file) {
        fflush(g_ota_file);
        fclose(g_ota_file);
        g_ota_file = NULL;
    }
    FILE *fcheck = fopen("/tmp/remote_ui.ota.gz", "rb");
    assert(fcheck != NULL);
    uint8_t written_data[2200];
    size_t rd = fread(written_data, 1, sizeof(written_data), fcheck);
    fclose(fcheck);
    assert(rd == test_size);
    assert(memcmp(written_data, synthetic_data, test_size) == 0);
    printf("[+] Step 6 passed: Written file matches synthetic test payload 100%% byte-for-byte!\n");

    /* Clean up */
    unlink("/tmp/remote_ui.ota.gz");
    unlink("/tmp/remote_ui.ota.meta");

    printf("\n[SUCCESS] All RF OTA idempotency, retransmission, and resume tests passed!\n");
    return 0;
}
