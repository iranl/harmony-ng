/* Going back one screen, by the Back key or a left-to-right swipe. Screens have no
 * Back rows: a confirmation goes back to its page, a Settings page to the categories, the
 * categories and the activity screen to home, and the device browser up a level. */
#include "remote_ui.h"
#include "remote_rf.h"
#include "remote_transition.h"

#include <string.h>

static bool go_back(void) {
    switch (g_current_view) {
        case VIEW_DEVICE_BROWSER:
            device_browser_back();
            return true;
        case VIEW_DEVICES:
            g_current_view = VIEW_ACTIVITIES;
            home_select_running();
            return true;
        case VIEW_PAIRING:
            if (g_unpair_confirm_active) {
                g_unpair_confirm_active = false;
            } else if (!is_remote_paired()) {
                g_current_view = VIEW_NOT_PAIRED;
            } else {
                g_current_view = VIEW_SETTINGS;
                g_settings_subview = SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = 0;
            }
            g_selected_pair_opt = 0;
            return true;
        case VIEW_SETTINGS:
            if (g_settings_subview == SETTINGS_VIEW_REBOOT || g_settings_subview == SETTINGS_VIEW_FACTORY_RESET ||
                g_settings_subview == SETTINGS_VIEW_SUSPEND) {
                bool was_reboot = g_settings_subview == SETTINGS_VIEW_REBOOT;
                bool was_suspend = g_settings_subview == SETTINGS_VIEW_SUSPEND;
                g_settings_subview = was_suspend ? SETTINGS_VIEW_POWER : SETTINGS_VIEW_SYSTEM;
                g_selected_sub_item = was_suspend ? 5 : (was_reboot ? 1 : 2);
            } else if (g_settings_subview != SETTINGS_VIEW_CATEGORIES) {
                g_selected_category = (int)g_settings_subview - SETTINGS_VIEW_ACTIVITIES;
                g_settings_subview = SETTINGS_VIEW_CATEGORIES;
            } else if (!is_remote_paired()) {
                g_current_view = VIEW_NOT_PAIRED;
            } else if (has_configured_activities()) {
                g_current_view = VIEW_ACTIVITIES;
                home_select_running();
            } else {
                open_device_browser();
            }
            g_factory_reset_stage = 0;
            g_suspend_countdown_start_ms = 0;
            return true;
        default:
            return false;
    }
}

bool ui_go_back(void) {
    if (!go_back()) return false;
    transition_hint_back();
    return true;
}
