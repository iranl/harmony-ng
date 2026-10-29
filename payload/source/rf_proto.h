#ifndef HARMONY_RF_PROTO_H
#define HARMONY_RF_PROTO_H

#include <ctype.h>
#include <stdint.h>
#include <string.h>

#define RF_REPORT_ID 0x20
#define RF_FRAME_SIZE 32
#define RF_PAYLOAD_SIZE 28

/* Message types */
/* Hub -> Remote */
#define RF_MSG_ACTIVITY_SYNC       0x0A  /* Current activity ID + name */
#define RF_MSG_ACTIVITY_LIST_CHUNK 0x0B  /* One activity entry (ACK'd) */
#define RF_MSG_DEVICE_LIST_CHUNK   0x0C  /* One device entry (ACK'd) */
#define RF_MSG_BUTTON_LIST_CHUNK   0x0D  /* One button entry (ACK'd) */
#define RF_MSG_SETTINGS_PUSH       0x0E  /* Remote display settings (ACK'd) */
#define RF_MSG_CONFIG_END          0x0F  /* Start/end config push (ACK'd) */
#define RF_MSG_TIME_SYNC           0x10  /* Time & Timezone sync (epoch + tz) */
#define RF_MSG_STEP_LIST_CHUNK     0x11  /* Activity sequence step + delay (ACK'd) */
#define RF_MSG_DEVICE_CMD_CHUNK    0x12  /* One device command name (ACK'd) */

/* Remote -> Hub (device browsing) */
#define RF_MSG_DEVICE_CMD_INDEX    0x13  /* Send device command by index + name prefix */

/* Remote -> Hub */
#define RF_MSG_BUTTON_PRESS        0x14  /* Physical button event */
#define RF_MSG_ACTIVITY_REQUEST    0x15  /* Start/stop activity */
#define RF_MSG_DEVICE_CMD_REQUEST  0x16  /* Send device command */
#define RF_MSG_SYNC_REQUEST        0x17  /* Ask Hub for current state */
#define RF_MSG_BATTERY_STATUS      0x18  /* Battery percent, charging state, voltage */
#define RF_MSG_REMOTE_CONFIG_BACKUP 0x19 /* Remote settings backup to Hub */
#define RF_MSG_VERSION_REPORT      0x1A  /* Remote running software & config version */
#define RF_MSG_UNPAIR_NOTIFY       0x1B  /* Hub <-> Remote: notify unpair before wiping bonding */
#define RF_MSG_CONFIG_PULL_REQ     0x1C  /* Hub -> Remote: request remote's current config/settings */
#define RF_MSG_SLEEP_REQ           0x1D  /* Hub -> Remote: trigger Suspend-to-RAM / deep sleep */

/* Bidirectional */
#define RF_MSG_ACK                 0x1E  /* Acknowledge by SEQ */
#define RF_MSG_PING                0x1F  /* Keepalive */

/* OTA Binary Update */
#define RF_MSG_OTA_START           0x22  /* Hub -> Remote: start binary OTA */
#define RF_MSG_OTA_CHUNK           0x23  /* Hub -> Remote: 24-byte binary chunk */
#define RF_MSG_OTA_BLOCK_ACK       0x24  /* Remote -> Hub: ACK per block */
#define RF_MSG_OTA_END             0x25  /* Hub -> Remote: complete / verify checksum */

/* Custom Icon Push (Hub -> Remote) */
#define RF_MSG_ICON_START          0x26  /* Hub -> Remote: start custom icon transfer */
#define RF_MSG_ICON_CHUNK          0x27  /* Hub -> Remote: 22-byte icon binary chunk */
#define RF_MSG_ICON_END            0x28  /* Hub -> Remote: complete / CRC16 verification */

/* Update Management */
#define RF_MSG_OTA_REQUEST         0x29  /* Remote -> Hub: request Hub to start binary OTA */
#define RF_MSG_UPDATE_NOTIFY       0x2A  /* Hub -> Remote: notify remote of pending config or OTA update */

/* Flags */
#define RF_FLAG_ACK_REQ  0x01
#define RF_FLAG_IS_ACK   0x02

typedef struct {
    uint8_t report_id;   /* Always 0x20 (CC2544 Very Long Report) */
    uint8_t dev_slot;    /* Destination/Source device slot (e.g. 0x01 / 0x02) */
    uint8_t sub_id;      /* Always 0x12 (CC2544 eQuad tunnel sub-ID required by firmware) */
    uint8_t seq;         /* Sequence number 0-255, wrapping */
    uint8_t hdr_flag;    /* 0x40 | (seq & 0x0F) or 0x60 | (seq & 0x0F) for CC2544 eQuad framing */
    uint8_t msg_type;    /* Message type (see RF_MSG_* constants) */
    uint8_t flags;       /* bit 0: ACK_REQ, bit 1: IS_ACK, bits 4-7: chunk_index */
    uint8_t payload[25]; /* Message-specific, zero-padded (25 bytes payload to keep 32 bytes total) */
} __attribute__((packed)) rf_frame_t;

/* Helper: build a frame */
static inline void rf_build_frame(rf_frame_t *f, uint8_t dev_slot, uint8_t type, uint8_t seq, uint8_t flags,
                                  const uint8_t *payload, size_t payload_len) {
    memset(f, 0, sizeof(*f));
    f->report_id = RF_REPORT_ID;
    f->dev_slot = dev_slot;
    f->sub_id = 0x12;
    f->seq = seq;
    f->hdr_flag = 0x50 | (seq & 0x0F);
    f->msg_type = type;
    f->flags = flags;
    if (payload && payload_len > 0) {
        if (payload_len > sizeof(f->payload)) payload_len = sizeof(f->payload);
        memcpy(f->payload, payload, payload_len);
    }
}

/* Helper: validate received frame */
static inline int rf_is_valid_frame(const uint8_t *raw, size_t len) {
    if (len >= 32 && raw[0] == RF_REPORT_ID && raw[2] == 0x12) {
        if ((raw[4] & 0xE0) == 0x40 || (raw[4] & 0xE0) == 0x60) {
            uint8_t t = raw[5];
            return (t >= RF_MSG_ACTIVITY_SYNC && t <= RF_MSG_DEVICE_CMD_INDEX) ||
                   (t >= RF_MSG_BUTTON_PRESS && t <= RF_MSG_SLEEP_REQ) ||
                   (t >= RF_MSG_OTA_START && t <= RF_MSG_UPDATE_NOTIFY) ||
                   (t == RF_MSG_ACK || t == RF_MSG_PING);
        }
        uint8_t t = raw[4];
        return (t >= RF_MSG_ACTIVITY_SYNC && t <= RF_MSG_DEVICE_CMD_INDEX) ||
               (t >= RF_MSG_BUTTON_PRESS && t <= RF_MSG_SLEEP_REQ) ||
               (t >= RF_MSG_OTA_START && t <= RF_MSG_UPDATE_NOTIFY) ||
               (t == RF_MSG_ACK || t == RF_MSG_PING);
    }
    if (len >= 30 && raw[0] == 0x12) {
        if ((raw[2] & 0xE0) == 0x40 || (raw[2] & 0xE0) == 0x60) {
            uint8_t t = raw[3];
            return (t >= RF_MSG_ACTIVITY_SYNC && t <= RF_MSG_DEVICE_CMD_INDEX) ||
                   (t >= RF_MSG_BUTTON_PRESS && t <= RF_MSG_SLEEP_REQ) ||
                   (t >= RF_MSG_OTA_START && t <= RF_MSG_UPDATE_NOTIFY) ||
                   (t == RF_MSG_ACK || t == RF_MSG_PING);
        }
        uint8_t t = raw[2];
        return (t >= RF_MSG_ACTIVITY_SYNC && t <= RF_MSG_DEVICE_CMD_INDEX) ||
               (t >= RF_MSG_BUTTON_PRESS && t <= RF_MSG_SLEEP_REQ) ||
               (t >= RF_MSG_OTA_START && t <= RF_MSG_UPDATE_NOTIFY) ||
               (t == RF_MSG_ACK || t == RF_MSG_PING);
    }
    return 0;
}

/* Helper: standard CRC16-CCITT for icon/asset validation */
static inline uint16_t rf_crc16(const uint8_t *data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
            else crc <<= 1;
        }
    }
    return crc;
}

/* Helper: build ACK frame */
static inline void rf_build_ack(rf_frame_t *f, uint8_t dev_slot, uint8_t acked_seq, uint8_t status) {
    memset(f, 0, sizeof(*f));
    f->report_id = RF_REPORT_ID;
    f->dev_slot = dev_slot;
    f->sub_id = 0x12;
    f->seq = acked_seq;
    f->hdr_flag = 0x40 | (acked_seq & 0x0F);
    f->msg_type = RF_MSG_ACK;
    f->flags = RF_FLAG_IS_ACK;
    f->payload[0] = acked_seq;
    f->payload[1] = status;
}

/* Activity icons are stored as keywords and sent over RF as a one-byte type (1..17).
   The remote turns the type back into the keyword and draws its themed icon for it. */
static inline const char *rf_activity_icon_keyword(uint8_t type) {
    static const char *const names[] = {
        "watchtv", "movie", "music", "game", "ps", "xbox", "switch", "shield", "apple",
        "roku", "firetv", "pc", "smarttv", "sonos", "receiver", "light", "power"
    };
    return (type >= 1 && type <= 17) ? names[type - 1] : names[0];
}

/* True if word occurs in s with no letter directly before or after it, so "ps" matches
   "PS5" and "ps.png" but not "Apps" or "Upstairs". */
static inline int rf_has_word(const char *s, const char *word) {
    size_t n = strlen(word);
    for (const char *p = strstr(s, word); p; p = strstr(p + 1, word)) {
        if ((p == s || !isalpha((unsigned char)p[-1])) && !isalpha((unsigned char)p[n])) return 1;
    }
    return 0;
}

/* Icon type for a keyword, activity name or Logitech activity type; 0 when nothing matches. */
static inline uint8_t rf_activity_icon_match(const char *s) {
    char low[64] = {0};
    for (int k = 0; s && s[k] && k < 63; k++) low[k] = (char)tolower((unsigned char)s[k]);
    if (!low[0]) return 0;
    /* Music before brands ("Apple Music"), brands before generic words ("Apple TV") */
    if (strstr(low, "music") || strstr(low, "audio") || strstr(low, "radio") || strstr(low, "spotify")) return 3;
    if (strstr(low, "apple")) return 9;
    if (strstr(low, "roku")) return 10;
    if (strstr(low, "firetv") || strstr(low, "amazon") || rf_has_word(low, "fire")) return 11;
    if (strstr(low, "shield")) return 8;
    if (strstr(low, "xbox")) return 6;
    if (strstr(low, "smarttv") || strstr(low, "smart tv")) return 13;
    if (strstr(low, "watchtv") || rf_has_word(low, "tv") || strstr(low, "television")) return 1;
    if (strstr(low, "movie") || strstr(low, "cinema") || strstr(low, "stream") || strstr(low, "netflix") ||
        strstr(low, "plex") || strstr(low, "dvd") || strstr(low, "blu")) return 2;
    if (strstr(low, "game") || strstr(low, "console")) return 4;
    if (rf_has_word(low, "ps") || strstr(low, "playstation")) return 5;
    if (strstr(low, "switch") || strstr(low, "nintendo")) return 7;
    if (rf_has_word(low, "pc") || strstr(low, "computer") || strstr(low, "laptop")) return 12;
    if (strstr(low, "sonos")) return 14;
    if (strstr(low, "receiver") || rf_has_word(low, "avr") || rf_has_word(low, "amp") ||
        strstr(low, "heos") || strstr(low, "denon")) return 15;
    if (strstr(low, "light") || strstr(low, "lamp") || strstr(low, "bulb")) return 16;
    if (strstr(low, "power") || rf_has_word(low, "off")) return 17;
    return 0;
}

/* Icon type for an activity. Accepts keywords and the stock Logitech image names that
   older web UIs stored; falls back to the activity name, then its type, then TV. */
static inline uint8_t rf_activity_icon_type(const char *icon, const char *name, const char *type) {
    static const struct { const char *guid; uint8_t type; } stock[] = {
        { "3bf99e7b-9904-4afa-8d3d-7337cd8e9bfd", 1 },
        { "bab09c4a-e61d-4284-b1f3-84135f562217", 2 },
        { "72d0aab4-2ed8-4644-84d2-0031bdae356e", 3 },
        { "dec07396-b858-47e8-ae50-4ab346af1d98", 5 },
        { "9d24eb35-468b-4148-a6a2-bb764aa9609c", 7 },
        { "0f40f257-68c8-4f24-9561-3cb3441da395", 9 },
        { "6ebd03b8-c815-48be-adbb-d36e06caac5e", 10 },
        { "5f5e0c9d-1d5a-4f6f-9dd4-243afccd8285", 11 },
        { "468668ab-2587-4662-9e89-cf66bd70f314", 12 },
    };
    if (icon && icon[0]) {
        const char *base = strrchr(icon, '/');
        base = base ? base + 1 : icon;
        char low[40] = {0};
        for (int k = 0; base[k] && k < 36; k++) low[k] = (char)tolower((unsigned char)base[k]);
        for (size_t i = 0; i < sizeof(stock) / sizeof(stock[0]); i++) {
            if (strcmp(low, stock[i].guid) == 0) return stock[i].type;
        }
        uint8_t t = rf_activity_icon_match(base);
        if (t) return t;
    }
    uint8_t t = rf_activity_icon_match(name);
    if (!t) t = rf_activity_icon_match(type);
    return t ? t : 1;
}

/* Short button name mapping (key_code -> 12-byte short name) */
static inline const char *rf_short_button_name(uint16_t key_code) {
    switch (key_code) {
        case 0x01EC: return "PwrOff";
        case 0x0052: return "Up";
        case 0x0051: return "Down";
        case 0x0050: return "Left";
        case 0x004F: return "Right";
        case 0x0058: return "Select";
        case 0x0225: return "Back";
        case 0x0065: return "Menu";
        case 0x0094: return "Exit";
        case 0x01FF: return "Info";
        case 0x008D: return "Guide";
        case 0x0089: return "Live";
        case 0x009A: return "Dvr";
        case 0x00E9: return "VolUp";
        case 0x00EA: return "VolDown";
        case 0x00E2: return "Mute";
        case 0x009C: return "ChUp";
        case 0x009D: return "ChDown";
        case 0x0224: return "PrevCh";
        case 0x00B0: return "Play";
        case 0x00B1: return "Pause";
        case 0x00B2: return "Record";
        case 0x00B3: return "FFwd";
        case 0x00B4: return "Rew";
        case 0x00B7: return "Stop";
        case 0x01F7: return "Red";
        case 0x01F6: return "Green";
        case 0x01F5: return "Yellow";
        case 0x01F4: return "Blue";
        case 0x0FF2: return "Ha1";
        case 0x0FF3: return "Ha2";
        case 0x0FF4: return "Ha3";
        case 0x0FF5: return "Ha4";
        case 0x0FF0: return "RockUp";
        case 0x0FF1: return "RockDn";
        default:     return "Unknown";
    }
}

#endif /* HARMONY_RF_PROTO_H */