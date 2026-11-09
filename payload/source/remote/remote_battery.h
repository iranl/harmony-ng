#ifndef REMOTE_BATTERY_H
#define REMOTE_BATTERY_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Battery percentage filter.
 *
 * Voltage to percentage: a curve through the stock firmware's own points for this remote and
 * PMIC (juniper/init.lua, style.lua, tde/testcharge.lua): shutdown below 3.55 V (0 %), the
 * battery icon steps at 3.70 V and 3.85 V, 3.9 V and up "fully charged", with the flat middle
 * of a Li-ion cell between them. The PMIC reads the cell under the remote's own standby load,
 * so a fresh cell rests near 3.90 V; the old linear 3.40-4.10 V map showed that as 72 %.
 * While charging the reading is high by the charge current times the cell's resistance, so it is
 * corrected first. How much depends on the charger: 0.17 V measured on the cradle (3.86 V
 * rested, 4.03 V charging), 0.10 V on USB power (3.644 V, 3.740 V); the stock factory test
 * (testcharge.lua) allows 0.2 V. A fixed 0.2 V made a nearly full remote on USB read 42 % after
 * a restart, and the value stayed low because it only rises while charging. So the offset is
 * measured when charging starts: the first charging reading minus the last rested reading on battery,
 * kept within REMOTE_BATT_CHARGE_OFFSET_MIN_MV..MAX_MV. A remote that starts while charging has
 * no reading on battery and uses REMOTE_BATT_CHARGE_OFFSET_MV.
 *
 * The cell sags under load: about 3.90 V at rest, 3.75 to 3.81 V a few seconds after the
 * screen turns on. Following every reading made the percentage walk down 2 points every couple
 * of seconds after each wake. So:
 *   - a rested reading (screen off long enough for the voltage to recover) sets the percentage,
 *     up or down;
 *   - otherwise, on battery, it never rises and falls by at most one point per
 *     REMOTE_BATT_DROP_STEP_MS, which follows a real discharge but not a load sag;
 *   - while charging it rises with the corrected reading and never falls (the charge only goes
 *     up on the cradle);
 *   - the first reading sets it; a change of the charging state keeps the last value (the
 *     reading right after it is taken under the load or the charge current) until a rested or
 *     charging reading moves it.
 *
 * The charging state itself is debounced: lifting the remote off the cradle or setting it down
 * flips it several times within seconds, so a change counts only after it held for
 * REMOTE_BATT_CHG_DEBOUNCE_MS.
 */

#define REMOTE_BATT_REST_MS 30000ULL         /* screen off this long = a rested reading */
#define REMOTE_BATT_DROP_STEP_MS 120000ULL   /* at most one point down per 2 minutes under load */
#define REMOTE_BATT_CHG_DEBOUNCE_MS 5000ULL  /* a charging change must hold this long */
#define REMOTE_BATT_CHARGE_OFFSET_MV 100     /* charging offset when none was measured (USB value) */
#define REMOTE_BATT_CHARGE_OFFSET_MIN_MV 50  /* a measured offset is kept within these */
#define REMOTE_BATT_CHARGE_OFFSET_MAX_MV 250

typedef struct {
    uint8_t pct;            /* 0xFF = no reading yet */
    uint64_t last_drop_ms;
    int last_batt_mv;       /* the last rested reading on battery, 0 = none yet */
    int chg_offset_mv;      /* how much a charging reading is high, measured at the last start */
    /* charging debounce */
    bool chg_known;
    bool chg_stable;        /* the debounced state */
    bool chg_pending;       /* the raw state that differs from chg_stable */
    uint64_t chg_pending_since;
} remote_batt_filter_t;

/* Percentage of a cell voltage read on battery (the curve above) */
int remote_battery_pct_of_mv(int mv);

void remote_battery_filter_init(remote_batt_filter_t *f);

/* Debounced charging state from the raw one; *changed is set when the debounced state changed
 * in this call (never on the first call) */
bool remote_battery_charging(remote_batt_filter_t *f, bool raw, uint64_t now_ms, bool *changed);

/* One step: avg_mv = the averaged reading, charging = the debounced state, chg_transition =
 * it changed in this sample, rested = the screen has been off for REMOTE_BATT_REST_MS. Returns
 * the percentage to report. */
uint8_t remote_battery_filter_step(remote_batt_filter_t *f, int avg_mv, bool charging,
                                  bool chg_transition, bool rested, uint64_t now_ms);

/* When the remote sends RF_MSG_BATTERY_STATUS: every 5 minutes, at once on a charging change,
 * with the screen on for a change of 2 points or more (at most once a minute), and at once when
 * requested: after the link came up and with every version report (a Hub that restarted then
 * gets the battery as soon as the remote is linked, not up to 5 minutes later). */
#define REMOTE_BATT_REPORT_INTERVAL_MS 300000ULL
#define REMOTE_BATT_REPORT_CHANGE_MS 60000ULL

typedef struct {
    bool sent;              /* a report went out since start */
    uint64_t last_ms;
    int last_pct;
    int last_chg;
    bool requested;
} remote_batt_report_t;

/* Ask for a report at the next chance (link up, version report) */
void remote_battery_request_report(remote_batt_report_t *r);
/* true when a report is due now; the caller sends it, this records it */
bool remote_battery_report_due(remote_batt_report_t *r, uint64_t now_ms, int pct, bool charging, bool screen_on);

/* The remote's reporter (remote_main.c checks it, remote_rf.c requests reports) */
extern remote_batt_report_t g_batt_report;

#endif
