#include "timezone_db.h"
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdio.h>

typedef struct {
    const char *name;
    const char *posix;
} tz_entry_t;

/*
 * Prefix expansion table for RF synchronization.
 * Hub transmits up to 14 bytes (string before comma).
 * Remote expands back to full POSIX string with exact DST transition rules.
 */
static const tz_entry_t s_prefix_mappings[] = {
    /* Europe */
    { "CET-1CEST",      "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "GMT0BST",        "GMT0BST,M3.5.0/1,M10.5.0" },
    { "WET0WEST",       "WET0WEST,M3.5.0/1,M10.5.0" },
    { "EET-2EEST",      "EET-2EEST,M3.5.0/3,M10.5.0/4" },

    /* North America */
    { "EST5EDT",        "EST5EDT,M3.2.0,M11.1.0" },
    { "CST6CDT",        "CST6CDT,M3.2.0,M11.1.0" },
    { "MST7MDT",        "MST7MDT,M3.2.0,M11.1.0" },
    { "PST8PDT",        "PST8PDT,M3.2.0,M11.1.0" },
    { "AKST9AKDT",      "AKST9AKDT,M3.2.0,M11.1.0" },
    { "NST3:30NDT",     "NST3:30NDT,M3.2.0,M11.1.0" },
    { "AST4ADT",        "AST4ADT,M3.2.0,M11.1.0" },
    { "CST5CDT",        "CST5CDT,M3.2.0/0,M11.1.0/1" },

    /* South America */
    { "BRT3BRST",       "BRT3BRST,M10.3.0/0,M2.3.0/0" },
    { "CLT4CLST",       "CLT4CLST,M9.1.6/24,M4.1.6/24" },
    { "PYT4PYST",       "PYT4PYST,M10.1.0,M3.4.0" },

    /* Middle East */
    { "IST-2IDT",       "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "IRST-3:30IRDT",  "IRST-3:30IRDT,M3.3.4,M9.3.4" },

    /* Australia & Pacific */
    { "AEST-10AEDT",    "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "ACST-9:30ACDT",  "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "LHST-10:30LHDT", "LHST-10:30LHDT-11,M10.1.0,M4.1.0/2" },
    { "NZST-12NZDT",    "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "CHAST-12:45CH",  "CHAST-12:45CHADT,M9.5.0/2:45,M4.1.0/3:45" },
};

/*
 * Canonical IANA timezone identifier to POSIX TZ string mapping.
 * Case-insensitive lookup supports standard REST API & web inputs.
 */
static const tz_entry_t s_iana_mappings[] = {
    /* UTC / GMT */
    { "UTC",                        "UTC0" },
    { "Etc/UTC",                    "UTC0" },
    { "Etc/GMT",                    "GMT0" },
    { "GMT",                        "GMT0" },

    /* Europe */
    { "Europe/Amsterdam",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Berlin",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Paris",               "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Rome",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Madrid",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Brussels",            "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Vienna",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zurich",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Warsaw",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Prague",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Budapest",            "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Stockholm",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Oslo",                "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Copenhagen",          "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Belgrade",            "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Bratislava",          "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Ljubljana",           "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/Zagreb",              "CET-1CEST,M3.5.0,M10.5.0/3" },
    { "Europe/London",              "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Dublin",              "GMT0BST,M3.5.0/1,M10.5.0" },
    { "Europe/Lisbon",              "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Canary",            "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Faroe",             "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Madeira",           "WET0WEST,M3.5.0/1,M10.5.0" },
    { "Atlantic/Reykjavik",         "GMT0" },
    { "Europe/Athens",              "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Helsinki",            "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Kyiv",                "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Bucharest",           "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Sofia",               "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Riga",                "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Tallinn",             "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Vilnius",             "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Chisinau",            "EET-2EEST,M3.5.0/3,M10.5.0/4" },
    { "Europe/Istanbul",            "TRT-3" },
    { "Europe/Minsk",               "MSK-3" },
    { "Europe/Moscow",              "MSK-3" },
    { "Europe/Simferopol",          "MSK-3" },
    { "Europe/Samara",              "SAMT-4" },
    { "Asia/Yekaterinburg",         "YEKT-5" },
    { "Asia/Omsk",                  "OMST-6" },
    { "Asia/Novosibirsk",           "NOVT-7" },
    { "Asia/Krasnoyarsk",           "KRAT-7" },
    { "Asia/Irkutsk",               "IRKT-8" },
    { "Asia/Yakutsk",               "YAKT-9" },
    { "Asia/Vladivostok",           "VLAT-10" },
    { "Asia/Magadan",               "MAGT-11" },
    { "Asia/Kamchatka",             "PETT-12" },

    /* North America */
    { "America/New_York",           "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Detroit",            "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Toronto",            "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Montreal",           "EST5EDT,M3.2.0,M11.1.0" },
    { "America/Chicago",            "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Winnipeg",           "CST6CDT,M3.2.0,M11.1.0" },
    { "America/Denver",             "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Edmonton",           "MST7MDT,M3.2.0,M11.1.0" },
    { "America/Phoenix",            "MST7" },
    { "America/Los_Angeles",        "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Vancouver",          "PST8PDT,M3.2.0,M11.1.0" },
    { "America/Anchorage",          "AKST9AKDT,M3.2.0,M11.1.0" },
    { "America/Juneau",             "AKST9AKDT,M3.2.0,M11.1.0" },
    { "Pacific/Honolulu",           "HST10" },
    { "America/Halifax",            "AST4ADT,M3.2.0,M11.1.0" },
    { "Atlantic/Bermuda",           "AST4ADT,M3.2.0,M11.1.0" },
    { "America/Puerto_Rico",        "AST4" },
    { "America/St_Johns",           "NST3:30NDT,M3.2.0,M11.1.0" },
    { "America/Mexico_City",        "CST6" },
    { "America/Cancun",             "EST5" },
    { "America/Havana",             "CST5CDT,M3.2.0/0,M11.1.0/1" },

    /* Central & South America */
    { "America/Costa_Rica",         "CST6" },
    { "America/Guatemala",          "CST6" },
    { "America/Panama",             "EST5" },
    { "America/Bogota",             "COT5" },
    { "America/Lima",               "PET5" },
    { "America/Guayaquil",          "ECT5" },
    { "America/Caracas",            "VET4" },
    { "America/La_Paz",             "BOT4" },
    { "America/Santiago",           "CLT4CLST,M9.1.6/24,M4.1.6/24" },
    { "America/Asuncion",           "PYT4PYST,M10.1.0,M3.4.0" },
    { "America/Sao_Paulo",          "BRT3" },
    { "America/Rio_de_Janeiro",     "BRT3" },
    { "America/Buenos_Aires",       "ART3" },
    { "America/Montevideo",         "UYT3" },

    /* Middle East */
    { "Asia/Jerusalem",             "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Tel_Aviv",              "IST-2IDT,M3.4.4/26,M10.5.0" },
    { "Asia/Beirut",                "EET-2EEST,M3.5.0/0,M10.5.0/0" },
    { "Asia/Amman",                 "AST-3" },
    { "Asia/Damascus",              "AST-3" },
    { "Asia/Baghdad",               "AST-3" },
    { "Asia/Riyadh",                "AST-3" },
    { "Asia/Kuwait",                "AST-3" },
    { "Asia/Qatar",                 "AST-3" },
    { "Asia/Bahrain",               "AST-3" },
    { "Asia/Dubai",                 "GST-4" },
    { "Asia/Muscat",                "GST-4" },
    { "Asia/Tehran",                "IRST-3:30" },

    /* Asia */
    { "Asia/Kabul",                 "AFT-4:30" },
    { "Asia/Karachi",               "PKT-5" },
    { "Asia/Tashkent",              "PKT-5" },
    { "Asia/Kolkata",               "IST-5:30" },
    { "Asia/Colombo",               "IST-5:30" },
    { "Asia/Kathmandu",             "NPT-5:45" },
    { "Asia/Dhaka",                 "BST-6" },
    { "Asia/Almaty",                "BST-6" },
    { "Asia/Yangon",                "MMT-6:30" },
    { "Asia/Bangkok",               "ICT-7" },
    { "Asia/Jakarta",               "WIB-7" },
    { "Asia/Ho_Chi_Minh",           "ICT-7" },
    { "Asia/Singapore",             "SGT-8" },
    { "Asia/Hong_Kong",             "HKT-8" },
    { "Asia/Shanghai",              "CST-8" },
    { "Asia/Taipei",                "CST-8" },
    { "Asia/Manila",                "PHT-8" },
    { "Asia/Kuala_Lumpur",          "MYT-8" },
    { "Asia/Tokyo",                 "JST-9" },
    { "Asia/Seoul",                 "KST-9" },

    /* Australia & Pacific */
    { "Australia/Perth",            "AWST-8" },
    { "Australia/Darwin",           "ACST-9:30" },
    { "Australia/Adelaide",         "ACST-9:30ACDT,M10.1.0,M4.1.0/3" },
    { "Australia/Brisbane",         "AEST-10" },
    { "Australia/Sydney",           "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Melbourne",        "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Hobart",           "AEST-10AEDT,M10.1.0,M4.1.0/3" },
    { "Australia/Lord_Howe",        "LHST-10:30LHDT-11,M10.1.0,M4.1.0/2" },
    { "Pacific/Auckland",           "NZST-12NZDT,M9.5.0,M4.1.0/3" },
    { "Pacific/Chatham",            "CHAST-12:45CHADT,M9.5.0/2:45,M4.1.0/3:45" },
    { "Pacific/Fiji",               "FJT-12" },
    { "Pacific/Guam",               "ChST-10" },

    /* Africa */
    { "Africa/Casablanca",          "GMT0" },
    { "Africa/Dakar",               "GMT0" },
    { "Africa/Abidjan",             "GMT0" },
    { "Africa/Accra",               "GMT0" },
    { "Africa/Lagos",               "WAT-1" },
    { "Africa/Kinshasa",            "WAT-1" },
    { "Africa/Luanda",              "WAT-1" },
    { "Africa/Johannesburg",        "CAT-2" },
    { "Africa/Cairo",               "EET-2EEST,M4.5.5/0,M10.5.4/24" },
    { "Africa/Harare",              "CAT-2" },
    { "Africa/Maputo",              "CAT-2" },
    { "Africa/Nairobi",             "EAT-3" },
    { "Africa/Addis_Ababa",         "EAT-3" },
};

void timezone_to_posix(const char *in, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    if (!in || !in[0]) {
        snprintf(out, out_sz, "%s", TZ_DEFAULT_POSIX);
        return;
    }

    /* Trim leading and trailing whitespace */
    while (*in && isspace((unsigned char)*in)) in++;
    char clean[64];
    strncpy(clean, in, sizeof(clean) - 1);
    clean[sizeof(clean) - 1] = '\0';
    char *end = clean + strlen(clean);
    while (end > clean && isspace((unsigned char)*(end - 1))) {
        *(--end) = '\0';
    }

    if (!clean[0]) {
        snprintf(out, out_sz, "%s", TZ_DEFAULT_POSIX);
        return;
    }

    /* 1. Check if input is a known IANA identifier */
    for (size_t i = 0; i < sizeof(s_iana_mappings) / sizeof(s_iana_mappings[0]); i++) {
        if (strcasecmp(clean, s_iana_mappings[i].name) == 0) {
            snprintf(out, out_sz, "%s", s_iana_mappings[i].posix);
            return;
        }
    }

    /* 2. Check if input matches a prefix in our RF/POSIX expansion table */
    for (size_t i = 0; i < sizeof(s_prefix_mappings) / sizeof(s_prefix_mappings[0]); i++) {
        if (strcasecmp(clean, s_prefix_mappings[i].name) == 0) {
            snprintf(out, out_sz, "%s", s_prefix_mappings[i].posix);
            return;
        }
    }

    /* 3. If it already contains comma rules (full POSIX TZ), preserve it */
    const char *comma1 = strchr(clean, ',');
    if (comma1 && strchr(comma1 + 1, ',')) {
        snprintf(out, out_sz, "%s", clean);
        return;
    }

    /* 4. If it's a fixed offset POSIX string (e.g. UTC, JST-9, MST7, MSK-3), preserve it */
    if (!strchr(clean, '/')) {
        snprintf(out, out_sz, "%s", clean);
        return;
    }

    /* 5. Fallback default */
    snprintf(out, out_sz, "%s", TZ_DEFAULT_POSIX);
}

void timezone_get_rf_prefix(const char *posix, char *out_prefix, size_t out_sz) {
    if (!out_prefix || out_sz == 0) return;
    out_prefix[0] = '\0';

    if (!posix || !posix[0]) {
        snprintf(out_prefix, out_sz, "CET-1CEST");
        return;
    }

    char buf[64];
    strncpy(buf, posix, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    /* Truncate at comma if present */
    char *comma = strchr(buf, ',');
    if (comma) {
        *comma = '\0';
    }

    /* Clamp to RF payload max length (14 bytes) */
    size_t len = strlen(buf);
    if (len > TZ_MAX_RF_PREFIX_LEN) {
        len = TZ_MAX_RF_PREFIX_LEN;
        buf[len] = '\0';
    }

    snprintf(out_prefix, out_sz, "%s", buf);
}

void timezone_expand_prefix(const char *prefix, char *out, size_t out_sz) {
    if (!out || out_sz == 0) return;

    if (!prefix || !prefix[0]) {
        snprintf(out, out_sz, "%s", TZ_DEFAULT_POSIX);
        return;
    }

    /* In-place buffer safety */
    if (prefix == out) {
        char tmp[64];
        timezone_expand_prefix(prefix, tmp, sizeof(tmp));
        snprintf(out, out_sz, "%s", tmp);
        return;
    }

    /* 1. If it already contains comma rules, it's already full POSIX */
    const char *c1 = strchr(prefix, ',');
    if (c1 && strchr(c1 + 1, ',')) {
        snprintf(out, out_sz, "%s", prefix);
        return;
    }

    /* 2. Match against prefix table */
    for (size_t i = 0; i < sizeof(s_prefix_mappings) / sizeof(s_prefix_mappings[0]); i++) {
        if (strncmp(prefix, s_prefix_mappings[i].name, strlen(s_prefix_mappings[i].name)) == 0) {
            snprintf(out, out_sz, "%s", s_prefix_mappings[i].posix);
            return;
        }
    }

    /* 3. If it has no comma, treat as fixed-offset (e.g. UTC, MSK-3, JST-9, MST7) */
    if (!strchr(prefix, ',')) {
        snprintf(out, out_sz, "%s", prefix);
        return;
    }

    snprintf(out, out_sz, "%s", TZ_DEFAULT_POSIX);
}
