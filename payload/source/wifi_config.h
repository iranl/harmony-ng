#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

/*
 * The Hub's Wi-Fi network (wpa_supplicant), shared by every writer: the web UI and API in
 * hub_daemon (POST /api/wifi-test, /api/wifi-save) and the USB setup in hub_util (usbwifi).
 *
 * - wifi_config_text(): the configuration file for one network, values written as typed
 *   (wpa_conf.h has the rules and refuses what the format cannot carry);
 * - wifi_config_test(): writes the candidate to both files, asks wpa_supplicant to use it (and
 *   starts wpa_supplicant on ath0 when it does not run, as network_manager.sh does) and waits
 *   up to 12 s for the link; on failure each file gets its previous content back, or is
 *   removed when it did not exist, and wpa_supplicant is pointed back at it, so the Hub stays
 *   on its old network (or on none, as before);
 * - while a test runs, copies of the previous files and a mark listing them are on flash next
 *   to the files: a test cut short (the process killed, the power cut) is undone by
 *   wifi_config_recover(), called when hub_daemon and the USB setup start and before every
 *   new test;
 * - wifi_config_save(): writes both files (both or neither) and, with apply, reconfigures.
 *
 * One test or apply at a time across processes: a lock file in /tmp with the holder's pid (a
 * pthread mutex cannot be shared with another program, and the Hub kernel has no futex, so
 * nothing here waits for a lock: a second caller is refused at once).
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WIFI_CONF_PATH "/etc/wpa_supplicant.conf"
#define WIFI_CONF_ATH0_PATH "/etc/wpa_supplicant.ath0.conf"
#define WIFI_CLI_PATH "/usr/sbin/wpa_cli"
#define WIFI_LOCK_PATH "/tmp/harmony_wifi_test.lock"
#define WIFI_SUPPLICANT_PATH "/usr/sbin/wpa_supplicant"
#define WIFI_WLANCONFIG_PATH "/sbin/wlanconfig"
#define WIFI_IFCONFIG_PATH "/sbin/ifconfig"
/* the previous files while a test runs; the mark ("conf=0|1 ath0=0|1": which files existed) is
 * written after both copies and removed first when the test ends */
#define WIFI_BACKUP_CONF_PATH "/etc/wpa_supplicant.conf.pretest"
#define WIFI_BACKUP_ATH0_PATH "/etc/wpa_supplicant.ath0.conf.pretest"
#define WIFI_TEST_MARK_PATH "/etc/wpa_supplicant.pretest"

/* 0, or -1 when buf is too small */
int wifi_config_text(char *buf, size_t len, const char *ssid, const char *psk, int is_open, int hidden);

/* 0 when the Hub associated with the network; -1 with the reason in err otherwise (nothing
 * changed then). Refused at once when another test or apply runs. */
int wifi_config_test(const char *ssid, const char *psk, int is_open, int hidden, char *err, size_t err_len);

/* Writes both wpa_supplicant files; apply 1 = then "wpa_cli reconfigure" in the background.
 * 0 or -1 (flash full: both files keep their old content). */
int wifi_config_save(const char *ssid, const char *psk, int is_open, int hidden, int apply);

/* Test, then save, under one lock (the USB setup): 0 when the network works and was saved;
 * -1 with the reason in err, the previous configuration kept. */
int wifi_config_apply(const char *ssid, const char *psk, int is_open, int hidden, char *err, size_t err_len);

struct wifi_status {
    int completed;          /* wpa_state=COMPLETED */
    char wpa_state[32];
    char ssid[64];
    char key_mgmt[32];
    char ip[32];            /* empty when ath0 has no address */
};

/* The live state from "wpa_cli -i ath0 status" (and ath0's address) */
void wifi_config_status(struct wifi_status *st);

/* Undoes a test that was cut short (see above): 1 when the previous files were put back, 0
 * when there was nothing to undo or a live test runs, -1 when the copies could not be read
 * (the mark is removed then, the files stay as they are). */
int wifi_config_recover(void);

/* The USB Wi-Fi setup switch in ethernet.conf (conf_path): 0 when it holds USB_WIFI_SETUP=0
 * (quotes and blanks allowed), 1 otherwise, also when the file or the key is missing (on by
 * default, as stock) */
int wifi_usb_setup_setting(const char *conf_path);

/* A path of the list above; test builds (TEST_HARNESS) put it under $HARMONY_WIFI_ROOT */
const char *wifi_config_path(const char *path, char *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_CONFIG_H */
