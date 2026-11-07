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
/* payload[4] of the OTA_START ACK and of every OTA data ACK from a remote that supports the
 * windowed transfer with RF_OTA_DATA frames (below); its ACKs then carry the number of contiguous
 * 26-byte chunks it has in payload[2..3], and the OTA_START ACK the chunk to resume from in
 * payload[6..7] (payload[2..3] stays the 22-byte chunk count for the stop-and-wait transfer). */
#define RF_OTA_WINDOWED            0xF1
/* OTA data frame, Hub -> Remote, windowed transfer only. It has no message type: like Logitech's
 * own HOT continuation packets it uses the header byte 0x60 and carries 26 data bytes in the
 * 30-byte Hub frame: 12 <slot> <0x60 | idx[3:0] | ack> <idx[11:4]> <data[26]>, where ack = 0x10
 * asks the remote for an ACK and the chunk index is 12 bits (the remote completes it from the
 * chunk it expects; 64 in flight is far below 4096). The last chunk is shorter: the remote knows
 * the image size from OTA_START. A remote accepts these only while in OTA mode. */
#define RF_OTA_HDR_DATA            0x60
#define RF_OTA_HDR_ACK             0x10
#define RF_OTA_DATA_BYTES          26
/* Chunk size of the stop-and-wait transfer (RF_MSG_OTA_CHUNK: index, length, 22 data bytes) */
#define RF_OTA_CHUNK_BYTES         22
/* Largest OTA image: chunk counts are 16-bit and the stop-and-wait chunks are the smallest */
#define RF_OTA_MAX_BYTES           (65535u * RF_OTA_CHUNK_BYTES)
/* CC2544 power modes (command 20 <mode> 00 00 00 00 00): Logitech's firmware runs the radio in
 * RF_PM_TRANSFER for the whole of a HOT transfer and in RF_PM_ACTIVE otherwise. In RF_PM_TRANSFER
 * the remote's radio fetches the Hub's queued frames by itself (about 250 a second on a Harmony
 * Ultimate); in RF_PM_ACTIVE it gets one only when it transmits. A remote puts the current mode
 * in byte 1 of every frame it sends (mode | pipe). */
#define RF_PM_ACTIVE               0x20
#define RF_PM_TRANSFER             0x30

/* Custom Icon Push (Hub -> Remote) */
#define RF_MSG_ICON_START          0x26  /* Hub -> Remote: start custom icon transfer */
#define RF_MSG_ICON_CHUNK          0x27  /* Hub -> Remote: 22-byte icon binary chunk */
#define RF_MSG_ICON_END            0x28  /* Hub -> Remote: complete / CRC16 verification */
#define RF_ICON_STATUS_OK              0x00  /* Ready for chunk streaming */
#define RF_ICON_STATUS_ALREADY_EXISTS  0x04  /* Remote already possesses exact icon */

/* Update Management */
#define RF_MSG_OTA_REQUEST         0x29  /* Remote -> Hub: request Hub to start binary OTA */
#define RF_MSG_UPDATE_NOTIFY       0x2A  /* Hub -> Remote: notify remote of pending config or OTA update */

/* Config push additions, sent only to remotes that report the matching capability */
#define RF_MSG_ACT_DEVICES_CHUNK   0x2B  /* Hub -> Remote: devices one activity uses (ACK'd) */
#define RF_MSG_NAME_TAIL_CHUNK     0x2C  /* Hub -> Remote: rest of an activity or device name (ACK'd) */

/* Help; the Hub sends RF_MSG_HELP_STATUS only in reply to a remote's request */
#define RF_MSG_HELP_REQUEST        0x2D  /* Remote -> Hub: repair the running activity */
#define RF_MSG_HELP_STATUS         0x2E  /* Hub -> Remote: Help progress, or a device to ask about (ACK'd) */
#define RF_MSG_HELP_ANSWER         0x2F  /* Remote -> Hub: whether a device is on (ACK'd) */

/* Hub -> Remote: turn the screen on (ACK'd). A remote in light standby still receives frames;
 * one without this message drops it as unknown and sends no ACK. */
#define RF_MSG_WAKE                0x30

/* Remote -> Hub: the remote's factory unit id (ACK'd). payload[0] = length, payload[1..] = the
 * id as ASCII (21 characters on current hardware, at most RF_UNIT_ID_MAX). Sent right after
 * every RF_MSG_VERSION_REPORT by remotes that report RF_CAP_UNIT_ID. */
#define RF_MSG_UNIT_ID             0x31
#define RF_UNIT_ID_MAX             24

/* RF_MSG_HELP_STATUS payload[0] */
#define RF_HELP_QUESTION           0x00  /* [1] index, [2] count, [3..10] device id, [11..24] name */
#define RF_HELP_STARTED            0x01  /* re-sending the activity's commands */
#define RF_HELP_BUSY               0x02  /* another activity change is running */
#define RF_HELP_FAILED             0x03  /* the activity is not running, or the Hub failed */
#define RF_HELP_NO_QUESTIONS       0x04  /* commands re-sent; no device has only a power toggle */

/* VERSION_REPORT payload[4]: what the remote's software understands (older remotes send 0) */
#define RF_CAP_ACT_DEVICES         0x01  /* RF_MSG_ACT_DEVICES_CHUNK */
#define RF_CAP_LONG_NAMES          0x02  /* RF_MSG_NAME_TAIL_CHUNK */
#define RF_CAP_UNIT_ID             0x04  /* sends RF_MSG_UNIT_ID after every version report */

/* Activity and device names, including the NUL, on remotes that report RF_CAP_LONG_NAMES */
#define RF_NAME_SIZE               40

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
                   (t >= RF_MSG_OTA_START && t <= RF_MSG_UNIT_ID) ||
                   (t == RF_MSG_ACK || t == RF_MSG_PING);
        }
        return 0;
    }
    if (len >= 30 && raw[0] == 0x12) {
        if ((raw[2] & 0xE0) == 0x40 || (raw[2] & 0xE0) == 0x60) {
            uint8_t t = raw[3];
            return (t >= RF_MSG_ACTIVITY_SYNC && t <= RF_MSG_DEVICE_CMD_INDEX) ||
                   (t >= RF_MSG_BUTTON_PRESS && t <= RF_MSG_SLEEP_REQ) ||
                   (t >= RF_MSG_OTA_START && t <= RF_MSG_UNIT_ID) ||
                   (t == RF_MSG_ACK || t == RF_MSG_PING);
        }
        return 0;
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

/* Helper: standard CRC32 for OTA binary verification / resume */
static inline uint32_t rf_crc32(const uint8_t *data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320 & (-(int)(crc & 1)));
        }
    }
    return ~crc;
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

/* Activity icons are stored as keywords and sent over RF as a one-byte type
   (1..RF_ACTIVITY_ICON_COUNT). The remote turns the type back into the keyword and draws its
   themed icon for it; a remote that doesn't know a type draws an icon from the name. New
   keywords go at the end, so types keep their meaning. */
#define RF_ACTIVITY_ICON_COUNT 27
static inline const char *rf_activity_icon_keyword(uint8_t type) {
    static const char *const names[RF_ACTIVITY_ICON_COUNT] = {
        "watchtv", "movie", "music", "game", "ps", "xbox", "switch", "shield", "apple",
        "roku", "firetv", "pc", "smarttv", "sonos", "receiver", "light", "power",
        "projector", "bluray", "satellite", "soundbar", "hifi", "netflix", "night", "fireplace",
        "sunrise", "fan"
    };
    return (type >= 1 && type <= RF_ACTIVITY_ICON_COUNT) ? names[type - 1] : names[0];
}

/* The built-in themes the Hub can choose in the settings push (payload[21], 1..4). 0 keeps the
   remote's own choice; other theme packages are chosen on the remote. */
static inline const char *rf_theme_name(uint8_t id) {
    static const char *const names[] = { "classic", "dark", "light", "colourful" };
    return (id >= 1 && id <= 4) ? names[id - 1] : NULL;
}

static inline uint8_t rf_theme_id(const char *name) {
    for (uint8_t id = 1; name && rf_theme_name(id); id++) {
        if (strcmp(name, rf_theme_name(id)) == 0) return id;
    }
    return 0;
}

/* The home layouts the Hub can choose in the settings push (payload[22], 1..4): the theme's,
   list, grid or carousel. 0 keeps the remote's own choice. */
static inline const char *rf_layout_name(uint8_t id) {
    static const char *const names[] = { "theme", "list", "grid", "carousel" };
    return (id >= 1 && id <= 4) ? names[id - 1] : NULL;
}

static inline uint8_t rf_layout_id(const char *name) {
    for (uint8_t id = 1; name && rf_layout_name(id); id++) {
        if (strcmp(name, rf_layout_name(id)) == 0) return id;
    }
    return 0;
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
    /* Audio equipment before music ("Soundbar Audio"), music before brands ("Apple Music"),
       brands and equipment before generic words ("Apple TV", "Projector TV") */
    if (strstr(low, "soundbar") || strstr(low, "sound bar")) return 21;
    if (strstr(low, "hifi") || strstr(low, "hi-fi") || strstr(low, "vinyl") || strstr(low, "turntable") ||
        strstr(low, "record player")) return 22;
    if (strstr(low, "music") || strstr(low, "audio") || strstr(low, "radio") || strstr(low, "spotify")) return 3;
    if (strstr(low, "netflix")) return 23;
    if (strstr(low, "apple")) return 9;
    if (strstr(low, "roku")) return 10;
    if (strstr(low, "fireplace")) return 25;
    if (strstr(low, "firetv") || strstr(low, "amazon") || rf_has_word(low, "fire")) return 11;
    if (strstr(low, "shield")) return 8;
    if (strstr(low, "xbox")) return 6;
    if (strstr(low, "smarttv") || strstr(low, "smart tv")) return 13;
    if (strstr(low, "projector") || strstr(low, "beamer")) return 18;
    if (strstr(low, "satellite") || rf_has_word(low, "sat") || strstr(low, "freesat") || rf_has_word(low, "dvb")) return 20;
    if (strstr(low, "watchtv") || rf_has_word(low, "tv") || strstr(low, "television")) return 1;
    if (strstr(low, "movie") || strstr(low, "cinema") || strstr(low, "stream") || strstr(low, "plex")) return 2;
    if (strstr(low, "dvd") || strstr(low, "blu") || rf_has_word(low, "disc")) return 19;
    if (strstr(low, "game") || strstr(low, "console")) return 4;
    if (rf_has_word(low, "ps") || strstr(low, "playstation")) return 5;
    if (strstr(low, "switch") || strstr(low, "nintendo")) return 7;
    if (rf_has_word(low, "pc") || strstr(low, "computer") || strstr(low, "laptop")) return 12;
    if (strstr(low, "sonos")) return 14;
    if (strstr(low, "receiver") || rf_has_word(low, "avr") || rf_has_word(low, "amp") ||
        strstr(low, "heos") || strstr(low, "denon")) return 15;
    if (strstr(low, "light") || strstr(low, "lamp") || strstr(low, "bulb")) return 16;
    if (strstr(low, "power") || rf_has_word(low, "off")) return 17;
    if (strstr(low, "night") || strstr(low, "sleep") || strstr(low, "bedtime") || strstr(low, "moon")) return 24;
    if (strstr(low, "sunrise") || strstr(low, "morning") || rf_has_word(low, "wake")) return 26;
    if (rf_has_word(low, "fan")) return 27;
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
        case 0x0224: return "Back";
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