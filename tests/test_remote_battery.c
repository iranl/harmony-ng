/* Battery percentage filter (payload/source/remote/remote_battery.c).
 * Replays the pattern a 2013 Ultimate showed on 2026-11-07: about 3.90 V at rest, a sag to
 * 3.81 V within seconds of the screen turning on, back at rest after the blank. */
#include <stdio.h>
#include <stdlib.h>
#include "remote_battery.h"

static int g_failures = 0;
#define CHECK(cond, msg) do { if (!(cond)) { g_failures++; printf("  FAIL %s (line %d)\n", msg, __LINE__); } } while (0)

/* The report rule of remote_main.c: every 5 minutes, on a charging change, or with the screen
 * on for a change of 2 points or more, at most once a minute */
typedef struct { uint64_t last_ms; int last_pct; int count; } reporter_t;
static void report_tick(reporter_t *r, int pct, bool screen_on, uint64_t now, bool limit_minute) {
    bool interval = (r->count == 0 || now - r->last_ms >= 300000);
    bool changed = (r->last_pct < 0 || abs(pct - r->last_pct) >= 2);
    bool due = screen_on && changed && (!limit_minute || now - r->last_ms >= 60000);
    if (interval || due) { r->last_ms = now; r->last_pct = pct; r->count++; }
}

/* The filter before this change: the linear 3.40-4.10 V map, follows every drop at once,
 * jumps up after a rise of 5 */
static int old_filter(int *f, int mv) {
    int pct = mv >= 4100 ? 100 : mv <= 3400 ? 0 : (mv - 3400) * 100 / 700;
    if (*f < 0) *f = pct;
    else if (pct < *f) *f = pct;
    else if (pct - *f > 4) *f = pct;
    return *f;
}

/* One wake: 40 s screen on (the voltage walks from rest to the sag over 16 s, like the
 * 16-sample average), then 120 s off. Returns the lowest and highest percentage seen. */
static void wake_cycle(remote_batt_filter_t *f, int *oldf, reporter_t *rn, reporter_t *ro, uint64_t *t,
                       int rest_mv, int sag_mv, int *lo, int *hi) {
    *lo = 101; *hi = -1;
    for (int s = 0; s < 160; s++, *t += 1000) {
        bool screen = s < 40;
        int k = screen ? s : (s - 40);
        int mv = screen ? (k >= 16 ? sag_mv : rest_mv - (rest_mv - sag_mv) * k / 16)
                        : (k >= 16 ? rest_mv : sag_mv + (rest_mv - sag_mv) * k / 16);
        bool rested = !screen && (s - 40) * 1000ULL >= REMOTE_BATT_REST_MS;
        int p = remote_battery_filter_step(f, mv, false, false, rested, *t);
        int po = old_filter(oldf, mv);
        report_tick(rn, p, screen, *t, true);
        report_tick(ro, po, screen, *t, false);
        if (p < *lo) *lo = p;
        if (p > *hi) *hi = p;
    }
}

int main(void) {
    printf("[*] Battery filter tests...\n");
    /* the curve through the stock firmware's points */
    CHECK(remote_battery_pct_of_mv(3500) == 0, "below the stock shutdown = 0 %");
    CHECK(remote_battery_pct_of_mv(3550) == 0, "3.55 V (stock shutdown) = 0 %");
    CHECK(remote_battery_pct_of_mv(3700) == 30, "3.70 V (stock low icon) = 30 %");
    CHECK(remote_battery_pct_of_mv(3850) == 75, "3.85 V (stock full icon) = 75 %");
    CHECK(remote_battery_pct_of_mv(3900) == 90, "3.90 V (stock: fully charged) = 90 %");
    CHECK(remote_battery_pct_of_mv(3950) == 100 && remote_battery_pct_of_mv(4200) == 100, "3.95 V and up = 100 %");
    CHECK(remote_battery_pct_of_mv(3908) == 91, "a fresh cell resting at 3.908 V reads 91 %, not 72 %");
    for (int mv = 3500, prev = -1; mv <= 4000; mv += 5) {
        int p = remote_battery_pct_of_mv(mv);
        if (p < prev) { CHECK(0, "the curve never falls with the voltage"); break; }
        prev = p;
    }

    /* 1. seven wakes in 20 minutes, rested 3.908 V, sagging to 3.810 V */
    remote_batt_filter_t f; remote_battery_filter_init(&f);
    int oldf = -1;
    reporter_t rn = {0, -1, 0}, ro = {0, -1, 0};
    uint64_t t = 1000;
    remote_battery_filter_step(&f, 3908, false, false, true, t);
    int lo = 0, hi = 0, worst_lo = 101;
    for (int i = 0; i < 7; i++) {
        wake_cycle(&f, &oldf, &rn, &ro, &t, 3908 - i * 2, 3810 - i * 2, &lo, &hi);
        if (lo < worst_lo) worst_lo = lo;
    }
    int start_pct = remote_battery_pct_of_mv(3908);
    printf("    7 wakes: start %d %%, lowest %d %%, reports new %d, old %d\n", start_pct, worst_lo, rn.count, ro.count);
    /* the rested voltage itself drops 2 mV per wake (the real discharge): compare with the
     * last wake's rested value, not the first */
    CHECK(worst_lo >= remote_battery_pct_of_mv(3908 - 6 * 2) - 1,
          "the sag moves the percentage by at most one point below the rested value");
    CHECK(rn.count <= 5, "about one report per 5 minutes, not one per sag step");
    CHECK(ro.count >= 30, "the old rule reported the sag (sanity check of the replay)");

    /* 2. a long session: 10 minutes at the sag voltage drops at most 5 points */
    remote_battery_filter_init(&f);
    t = 0;
    int p0 = remote_battery_filter_step(&f, 3908, false, false, true, t);
    int p = 0;
    for (int s = 1; s <= 600; s++) p = remote_battery_filter_step(&f, 3810, false, false, false, t + s * 1000ULL);
    CHECK(p >= p0 - 5 && p <= p0 - 4, "10 minutes under load: 4 to 5 points down, not the 30 of the sag");
    /* back at rest: the rested voltage sets the percentage again, up included */
    p = remote_battery_filter_step(&f, 3900, false, false, true, t + 700000);
    CHECK(p == remote_battery_pct_of_mv(3900), "a rested reading sets the percentage, up or down");
    p = remote_battery_filter_step(&f, 3880, false, false, true, t + 701000);
    CHECK(p == remote_battery_pct_of_mv(3880), "rested readings follow a real discharge at once");

    /* 3. under load the percentage never rises */
    p = remote_battery_filter_step(&f, 4000, false, false, false, t + 702000);
    CHECK(p == remote_battery_pct_of_mv(3880), "no rise under load");

    /* 4. charging on USB (Harmony Ultimate: 3.644 V rested, 3.740 V with the charger on): the
     *    offset is measured at the start, the percentage rises with the corrected reading and
     *    never falls */
    remote_battery_filter_init(&f);
    p = remote_battery_filter_step(&f, 3644, false, false, true, 0);
    int p_rest = p;
    p = remote_battery_filter_step(&f, 3740, true, true, false, 1000);
    CHECK(p == p_rest && f.chg_offset_mv == 96, "the charging start keeps the value and measures 96 mV");
    p = remote_battery_filter_step(&f, 3746, true, false, false, 2000);
    CHECK(p == remote_battery_pct_of_mv(3650), "charging: corrected by the measured offset");
    p = remote_battery_filter_step(&f, 4000, true, false, false, 3000);
    CHECK(p == remote_battery_pct_of_mv(3904), "charging: rises at once");
    p = remote_battery_filter_step(&f, 3950, true, false, false, 4000);
    CHECK(p == remote_battery_pct_of_mv(3904), "charging: never falls");
    /* a start while charging: restarted on USB after an hour of charging it read 3.94 V, then
     * 4.00 V; with a fixed 0.2 V that was 42 % and then 60 % */
    remote_battery_filter_init(&f);
    p = remote_battery_filter_step(&f, 3940, true, false, false, 0);
    CHECK(p == remote_battery_pct_of_mv(3840), "a start while charging uses the default offset");
    p = remote_battery_filter_step(&f, 4000, true, false, false, 60000);
    CHECK(p >= 85, "nearly full on USB is not shown as 60 %");
    /* a measured offset stays within the limits */
    remote_battery_filter_init(&f);
    remote_battery_filter_step(&f, 3600, false, false, true, 0);
    remote_battery_filter_step(&f, 4100, true, true, false, 1000);
    CHECK(f.chg_offset_mv == REMOTE_BATT_CHARGE_OFFSET_MAX_MV, "offset capped");
    remote_battery_filter_init(&f);
    remote_battery_filter_step(&f, 3800, false, false, true, 0);
    remote_battery_filter_step(&f, 3790, true, true, false, 1000);
    CHECK(f.chg_offset_mv == REMOTE_BATT_CHARGE_OFFSET_MIN_MV, "offset floored");

    /* 5. the charging state is debounced: off, on, off within 9 s (lifting the remote) is one
     *    change, 5 s after it settled */
    remote_battery_filter_init(&f);
    int changes = 0, change_at = -1;
    bool c = remote_battery_charging(&f, true, 0, NULL);
    CHECK(c, "first state taken as it is");
    const bool raw_lift[] = { 1, 1, 1, 0, 0, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    for (int s = 0; s < (int)(sizeof(raw_lift) / sizeof(raw_lift[0])); s++) {
        bool ch = false;
        c = remote_battery_charging(&f, raw_lift[s], (uint64_t)(s + 1) * 1000, &ch);
        if (ch) { changes++; change_at = s; }
    }
    CHECK(changes == 1 && !c, "the bounce gives one change, to not charging");
    CHECK(change_at == 8 + 5, "the change counts 5 s after the last flip");

    /* 6. the 10 s blip at 04:05 and the lift at 03:50: the percentage stays put through the
     *    charging changes (it went 84, 65, 74 and 80, 94, 74), a rested reading moves it */
    remote_battery_filter_init(&f);
    t = 0;
    bool chg = remote_battery_charging(&f, false, t, NULL);
    p = remote_battery_filter_step(&f, 3860, chg, false, true, t); /* rested on battery */
    int before = p;
    const bool raw_blip[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    int lo2 = 101, hi2 = -1, transitions = 0;
    for (int s = 0; s < (int)(sizeof(raw_blip) / sizeof(raw_blip[0])); s++) {
        t += 1000;
        bool ch = false;
        chg = remote_battery_charging(&f, raw_blip[s], t, &ch);
        if (ch) transitions++;
        /* the loaded or charging reading: 3.79 V on battery, 4.03 V while the charger is on */
        p = remote_battery_filter_step(&f, raw_blip[s] ? 4030 : 3790, chg, ch, false, t);
        if (p < lo2) lo2 = p;
        if (p > hi2) hi2 = p;
    }
    printf("    10 s charging blip: before %d %%, range %d..%d %%, %d transitions\n", before, lo2, hi2, transitions);
    CHECK(transitions == 2, "the 10 s blip is a charging change and back");
    CHECK(lo2 >= before - 1 && hi2 <= before + 1, "the percentage stays put through the blip");
    p = remote_battery_filter_step(&f, 3850, false, false, true, t + 60000);
    CHECK(p == remote_battery_pct_of_mv(3850), "the next rested reading sets it");
    /* the cradle blip measured its own offset: 4.03 V charging against the 3.86 V rested reading */
    CHECK(f.chg_offset_mv == 170, "cradle offset measured at the blip");

    /* 7. the reporter: every 5 minutes, at once on request (link up, version report), on a
     *    charging change, and with the screen on for 2 points (at most once a minute) */
    remote_batt_report_t r = { false, 0, -1, -1, false };
    CHECK(remote_battery_report_due(&r, 1000, 80, false, false), "the first report goes out at once");
    CHECK(!remote_battery_report_due(&r, 61000, 80, false, false), "nothing new within 5 minutes");
    remote_battery_request_report(&r);
    CHECK(remote_battery_report_due(&r, 62000, 80, false, false), "a requested report goes out at once (a Hub that restarted)");
    CHECK(!remote_battery_report_due(&r, 63000, 80, false, false), "a request is served once");
    CHECK(remote_battery_report_due(&r, 64000, 80, true, false), "a charging change goes out at once");
    CHECK(!remote_battery_report_due(&r, 65000, 77, true, true), "screen on, 3 points, but under a minute since the last");
    CHECK(remote_battery_report_due(&r, 125000, 77, true, true), "screen on, 3 points, a minute later");
    CHECK(!remote_battery_report_due(&r, 400000, 77, true, false), "screen off and no change: the interval");
    CHECK(remote_battery_report_due(&r, 425000, 77, true, false), "the 5-minute interval");

    if (g_failures) { printf("[-] %d battery filter checks failed\n", g_failures); return 1; }
    printf("[+] All battery filter checks passed\n");
    return 0;
}
