#ifndef ELITE_I18N_H
#define ELITE_I18N_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ELITE_LANG_EN = 0,
    ELITE_LANG_DE,
    ELITE_LANG_FR,
    ELITE_LANG_NL,
    ELITE_LANG_ES,
    ELITE_LANG_IT,
    ELITE_LANG_COUNT
} elite_lang_t;

typedef enum {
    /* Headers & views */
    STR_ACTIVITIES = 0,
    STR_DEVICES,
    STR_COMMANDS,
    STR_SETTINGS,
    STR_RF_PAIRING,
    STR_NOT_PAIRED,
    STR_NO_HUB,
    STR_TAB_ACT,
    STR_TAB_CMD,
    STR_TAB_DEV,
    STR_BACK,
    STR_BACK_TO_SETTINGS,

    /* Activities screen */
    STR_NO_ACTIVITIES,
    STR_SYNCING_WITH_HUB,
    STR_CONFIRM_PRESS_OK,
    STR_ACTIVE,
    STR_ACT_ALREADY_ACTIVE,
    STR_PRESS_OK_TO_CONFIRM,

    /* Switching overlay */
    STR_SWITCHING_ACTIVITY,
    STR_SWITCHING_TO,
    STR_CONFIGURING_DEVICES,
    STR_POWERING_OFF_ALL,
    STR_EXECUTING_ACTIONS,
    STR_PLEASE_WAIT,
    STR_SCREEN_LOCKED_SWITCH,

    /* Commands & Device browser */
    STR_NO_DEVICES,
    STR_NO_COMMANDS,
    STR_CONFIGURE_IN_WEBUI,
    STR_FOR_THIS_ACTIVITY,

    /* Sync overlay */
    STR_SYNC_CONFIG_TITLE,
    STR_UPDATING_FROM_HUB,
    STR_SYNCING_ACTS_MAPS,
    STR_SCREEN_LOCKED_SYNC,

    /* OTA overlay */
    STR_OTA_TITLE,
    STR_UPDATING_FIRMWARE,
    STR_RECEIVING_OVER_RF,
    STR_DO_NOT_POWER_OFF,
    STR_KEEP_CLOSE_TO_HUB,
    STR_UPDATE_IN_PROGRESS,

    /* Pairing screen */
    STR_RF_HARDWARE_LINK,
    STR_STATUS_PAIRED,
    STR_STATUS_NOT_PAIRED,
    STR_STATUS_PAIRING,
    STR_STATUS_FAILED,
    STR_UNPAIR_CONFIRM,
    STR_WIPE_BONDING_CONF,
    STR_CONFIRM_UNPAIR,
    STR_CANCEL,
    STR_STOP_PAIRING,
    STR_UNPAIR_FROM_HUB,
    STR_START_PAIRING,
    STR_REMOTE_NOT_PAIRED,
    STR_CONNECT_WITH_HUB,
    STR_CONTROL_ACTS_DEVS,
    STR_TAP_BUTTON_TO_PAIR,
    STR_PAIR_TO_HUB,

    /* Settings categories */
    STR_CAT_CONFIG,
    STR_CAT_CONFIG_DESC,
    STR_CAT_DISPLAY,
    STR_CAT_DISPLAY_DESC,
    STR_CAT_TOUCH,
    STR_CAT_TOUCH_DESC,
    STR_CAT_POWER,
    STR_CAT_POWER_DESC,
    STR_CAT_SYSTEM,
    STR_CAT_SYSTEM_DESC,
    STR_CAT_UPDATE,
    STR_CAT_UPDATE_DESC,

    /* Settings CONFIG subview */
    STR_CONFIRM_SWITCH,
    STR_CONFIRM_PROMPT,
    STR_CONFIRM_INSTANT,
    STR_LANGUAGE,
    STR_SYNC_CONFIG_HUB,

    /* Settings DISPLAY subview */
    STR_DISPLAY_BRIGHTNESS,
    STR_DISPLAY_TIMEOUT,
    STR_BTN_BACKLIGHT,
    STR_NIGHT_SCHEDULE,

    /* Settings TOUCH subview */
    STR_TOUCHSCREEN,
    STR_CAP_BUTTONS,
    STR_TOUCH_SENSITIVITY,
    STR_NAV_TOGGLE_BTN,
    STR_NAV_TOGGLE_MODE,

    /* Settings POWER subview */
    STR_TILT_WAKE,
    STR_GYRO_RATE,
    STR_BATTERY_LEVEL,
    STR_BATTERY_VOLTAGE,
    STR_SUSPEND_RAM,

    /* Settings SYSTEM subview */
    STR_USB_ETHERNET,
    STR_DEBUG_LOGGING,
    STR_RF_PAIRING_LINK,
    STR_REBOOT_REMOTE,
    STR_FACTORY_RESET,

    /* Settings UPDATE subview */
    STR_PULL_FIRMWARE_OTA,
    STR_CHECK_UPDATES,

    /* Confirmation dialogues */
    STR_CANCEL_KEEP_SYSTEM,
    STR_PROCEED_TO_RESET,
    STR_NO_GO_BACK,
    STR_YES_RESET_REMOTE,
    STR_CANCEL_DO_NOT_REBOOT,
    STR_REBOOT_REMOTE_NOW,
    STR_CANCEL_STAY_AWAKE,
    STR_SUSPEND_TO_RAM_NOW,

    /* Notices / toasts */
    STR_NOTICE_NAV_MOVED,
    STR_NOTICE_NAV_LOCKED,
    STR_NOTICE_HUB_NOT_CONNECTED,
    STR_NOTICE_SYNCING,
    STR_NOTICE_REBOOTING,
    STR_NOTICE_RESTARTING,
    STR_NOTICE_RESTORING,
    STR_NOTICE_LANG_CHANGED,

    STR_COUNT
} elite_str_id_t;

extern elite_lang_t g_language;

const char *tr(elite_str_id_t id);
const char *tr_lang(elite_str_id_t id, elite_lang_t lang);
const char *elite_lang_code(elite_lang_t lang);
const char *elite_lang_name(elite_lang_t lang);
elite_lang_t elite_lang_from_code(const char *code);
void elite_lang_cycle(void);

#ifdef __cplusplus
}
#endif

#endif /* ELITE_I18N_H */
