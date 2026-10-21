/* tz_win32.c - time zones on Windows for std.time (platform.h
 * slang__plat_tz_tzif): Windows has no tzdata files, so the zone rules come
 * from the registry (HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\
 * Time Zones\<key>: TZI = bias + the yearly DST rule, "Dynamic DST" = one
 * rule per year) and are turned into TZif (RFC 8536 version 2) bytes that
 * time.sl parses like a file from /usr/share/zoneinfo: one transition per
 * DST change for the years listed under "Dynamic DST", and the current
 * rule as the POSIX TZ footer for later years.
 *
 * The local zone is GetDynamicTimeZoneInformation's TimeZoneKeyName (the
 * "adjust for daylight saving" switch off = standard time all year).
 * IANA names (LoadLocation("Europe/Berlin")) are mapped to Windows keys
 * with the table below: CLDR's windowsZones.xml, territory 001, for the
 * common zones, plus frequent aliases. Abbreviations ("CET"/"CEST") come
 * from the table too (Windows only has long names); zones without
 * well-known letters use numeric ones ("+03"), as tzdata does. History
 * before the first "Dynamic DST" year is not known: earlier instants use
 * that year's standard offset. $ZONEINFO pointing at a tzdata directory
 * takes precedence (std_time_zone.c, time.sl). */
#include "platform.h"

#if defined(_WIN32)

#include "win32_util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct tz_map {
    const char *win;   /* registry key under Time Zones */
    const char *iana;  /* CLDR 001 zone (first entry of a key) or an alias */
    const char *std, *dst;  /* abbreviations, NULL = numeric */
} tz_map;

static const tz_map g_map[] = {
    { "Dateline Standard Time", "Etc/GMT+12", NULL, NULL },
    { "UTC-11", "Etc/GMT+11", NULL, NULL },
    { "Aleutian Standard Time", "America/Adak", "HST", "HDT" },
    { "Hawaiian Standard Time", "Pacific/Honolulu", "HST", NULL },
    { "Marquesas Standard Time", "Pacific/Marquesas", NULL, NULL },
    { "Alaskan Standard Time", "America/Anchorage", "AKST", "AKDT" },
    { "UTC-09", "Etc/GMT+9", NULL, NULL },
    { "Pacific Standard Time (Mexico)", "America/Tijuana", "PST", "PDT" },
    { "UTC-08", "Etc/GMT+8", NULL, NULL },
    { "Pacific Standard Time", "America/Los_Angeles", "PST", "PDT" },
    { "Pacific Standard Time", "America/Vancouver", "PST", "PDT" },
    { "US Mountain Standard Time", "America/Phoenix", "MST", NULL },
    { "Mountain Standard Time (Mexico)", "America/Mazatlan", "MST", NULL },
    { "Mountain Standard Time", "America/Denver", "MST", "MDT" },
    { "Mountain Standard Time", "America/Edmonton", "MST", "MDT" },
    { "Yukon Standard Time", "America/Whitehorse", "MST", NULL },
    { "Central America Standard Time", "America/Guatemala", "CST", NULL },
    { "Central Standard Time", "America/Chicago", "CST", "CDT" },
    { "Central Standard Time", "America/Winnipeg", "CST", "CDT" },
    { "Easter Island Standard Time", "Pacific/Easter", NULL, NULL },
    { "Central Standard Time (Mexico)", "America/Mexico_City", "CST", NULL },
    { "Canada Central Standard Time", "America/Regina", "CST", NULL },
    { "SA Pacific Standard Time", "America/Bogota", NULL, NULL },
    { "SA Pacific Standard Time", "America/Lima", NULL, NULL },
    { "Eastern Standard Time (Mexico)", "America/Cancun", "EST", NULL },
    { "Eastern Standard Time", "America/New_York", "EST", "EDT" },
    { "Eastern Standard Time", "America/Toronto", "EST", "EDT" },
    { "Eastern Standard Time", "America/Detroit", "EST", "EDT" },
    { "Haiti Standard Time", "America/Port-au-Prince", "EST", "EDT" },
    { "Cuba Standard Time", "America/Havana", "CST", "CDT" },
    { "US Eastern Standard Time", "America/Indianapolis", "EST", "EDT" },
    { "US Eastern Standard Time", "America/Indiana/Indianapolis", "EST", "EDT" },
    { "Turks And Caicos Standard Time", "America/Grand_Turk", "EST", "EDT" },
    { "Paraguay Standard Time", "America/Asuncion", NULL, NULL },
    { "Atlantic Standard Time", "America/Halifax", "AST", "ADT" },
    { "Venezuela Standard Time", "America/Caracas", NULL, NULL },
    { "Central Brazilian Standard Time", "America/Cuiaba", NULL, NULL },
    { "SA Western Standard Time", "America/La_Paz", NULL, NULL },
    { "Pacific SA Standard Time", "America/Santiago", NULL, NULL },
    { "Newfoundland Standard Time", "America/St_Johns", "NST", "NDT" },
    { "Tocantins Standard Time", "America/Araguaina", NULL, NULL },
    { "E. South America Standard Time", "America/Sao_Paulo", NULL, NULL },
    { "SA Eastern Standard Time", "America/Cayenne", NULL, NULL },
    { "Argentina Standard Time", "America/Buenos_Aires", NULL, NULL },
    { "Argentina Standard Time", "America/Argentina/Buenos_Aires", NULL, NULL },
    { "Greenland Standard Time", "America/Godthab", NULL, NULL },
    { "Greenland Standard Time", "America/Nuuk", NULL, NULL },
    { "Montevideo Standard Time", "America/Montevideo", NULL, NULL },
    { "Magallanes Standard Time", "America/Punta_Arenas", NULL, NULL },
    { "Saint Pierre Standard Time", "America/Miquelon", NULL, NULL },
    { "Bahia Standard Time", "America/Bahia", NULL, NULL },
    { "UTC-02", "Etc/GMT+2", NULL, NULL },
    { "Azores Standard Time", "Atlantic/Azores", NULL, NULL },
    { "Cape Verde Standard Time", "Atlantic/Cape_Verde", NULL, NULL },
    { "UTC", "Etc/UTC", "UTC", NULL },
    { "UTC", "Etc/GMT", "GMT", NULL },
    { "UTC", "GMT", "GMT", NULL },
    { "GMT Standard Time", "Europe/London", "GMT", "BST" },
    { "GMT Standard Time", "Europe/Dublin", "GMT", "IST" },
    { "GMT Standard Time", "Europe/Lisbon", "WET", "WEST" },
    { "Greenwich Standard Time", "Atlantic/Reykjavik", "GMT", NULL },
    { "Sao Tome Standard Time", "Africa/Sao_Tome", "GMT", NULL },
    { "Morocco Standard Time", "Africa/Casablanca", NULL, NULL },
    { "W. Europe Standard Time", "Europe/Berlin", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Amsterdam", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Rome", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Stockholm", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Vienna", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Zurich", "CET", "CEST" },
    { "W. Europe Standard Time", "Europe/Oslo", "CET", "CEST" },
    { "Central Europe Standard Time", "Europe/Budapest", "CET", "CEST" },
    { "Central Europe Standard Time", "Europe/Prague", "CET", "CEST" },
    { "Central Europe Standard Time", "Europe/Belgrade", "CET", "CEST" },
    { "Romance Standard Time", "Europe/Paris", "CET", "CEST" },
    { "Romance Standard Time", "Europe/Madrid", "CET", "CEST" },
    { "Romance Standard Time", "Europe/Brussels", "CET", "CEST" },
    { "Romance Standard Time", "Europe/Copenhagen", "CET", "CEST" },
    { "Central European Standard Time", "Europe/Warsaw", "CET", "CEST" },
    { "Central European Standard Time", "Europe/Zagreb", "CET", "CEST" },
    { "W. Central Africa Standard Time", "Africa/Lagos", "WAT", NULL },
    { "W. Central Africa Standard Time", "Africa/Algiers", "CET", NULL },
    { "Jordan Standard Time", "Asia/Amman", NULL, NULL },
    { "GTB Standard Time", "Europe/Bucharest", "EET", "EEST" },
    { "GTB Standard Time", "Europe/Athens", "EET", "EEST" },
    { "Middle East Standard Time", "Asia/Beirut", "EET", "EEST" },
    { "Egypt Standard Time", "Africa/Cairo", "EET", "EEST" },
    { "E. Europe Standard Time", "Europe/Chisinau", "EET", "EEST" },
    { "Syria Standard Time", "Asia/Damascus", NULL, NULL },
    { "West Bank Standard Time", "Asia/Hebron", "EET", "EEST" },
    { "South Africa Standard Time", "Africa/Johannesburg", "SAST", NULL },
    { "FLE Standard Time", "Europe/Kiev", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Kyiv", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Helsinki", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Riga", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Sofia", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Tallinn", "EET", "EEST" },
    { "FLE Standard Time", "Europe/Vilnius", "EET", "EEST" },
    { "Israel Standard Time", "Asia/Jerusalem", "IST", "IDT" },
    { "South Sudan Standard Time", "Africa/Juba", "CAT", NULL },
    { "Kaliningrad Standard Time", "Europe/Kaliningrad", "EET", NULL },
    { "Sudan Standard Time", "Africa/Khartoum", "CAT", NULL },
    { "Libya Standard Time", "Africa/Tripoli", "EET", NULL },
    { "Namibia Standard Time", "Africa/Windhoek", "CAT", NULL },
    { "Arabic Standard Time", "Asia/Baghdad", NULL, NULL },
    { "Turkey Standard Time", "Europe/Istanbul", NULL, NULL },
    { "Arab Standard Time", "Asia/Riyadh", NULL, NULL },
    { "Belarus Standard Time", "Europe/Minsk", NULL, NULL },
    { "Russian Standard Time", "Europe/Moscow", "MSK", NULL },
    { "E. Africa Standard Time", "Africa/Nairobi", "EAT", NULL },
    { "Volgograd Standard Time", "Europe/Volgograd", NULL, NULL },
    { "Iran Standard Time", "Asia/Tehran", NULL, NULL },
    { "Arabian Standard Time", "Asia/Dubai", NULL, NULL },
    { "Astrakhan Standard Time", "Europe/Astrakhan", NULL, NULL },
    { "Azerbaijan Standard Time", "Asia/Baku", NULL, NULL },
    { "Russia Time Zone 3", "Europe/Samara", NULL, NULL },
    { "Mauritius Standard Time", "Indian/Mauritius", NULL, NULL },
    { "Saratov Standard Time", "Europe/Saratov", NULL, NULL },
    { "Georgian Standard Time", "Asia/Tbilisi", NULL, NULL },
    { "Caucasus Standard Time", "Asia/Yerevan", NULL, NULL },
    { "Afghanistan Standard Time", "Asia/Kabul", NULL, NULL },
    { "West Asia Standard Time", "Asia/Tashkent", NULL, NULL },
    { "Ekaterinburg Standard Time", "Asia/Yekaterinburg", NULL, NULL },
    { "Pakistan Standard Time", "Asia/Karachi", "PKT", NULL },
    { "Qyzylorda Standard Time", "Asia/Qyzylorda", NULL, NULL },
    { "India Standard Time", "Asia/Calcutta", "IST", NULL },
    { "India Standard Time", "Asia/Kolkata", "IST", NULL },
    { "Sri Lanka Standard Time", "Asia/Colombo", NULL, NULL },
    { "Nepal Standard Time", "Asia/Katmandu", NULL, NULL },
    { "Nepal Standard Time", "Asia/Kathmandu", NULL, NULL },
    { "Central Asia Standard Time", "Asia/Almaty", NULL, NULL },
    { "Central Asia Standard Time", "Asia/Bishkek", NULL, NULL },
    { "Bangladesh Standard Time", "Asia/Dhaka", NULL, NULL },
    { "Omsk Standard Time", "Asia/Omsk", NULL, NULL },
    { "Myanmar Standard Time", "Asia/Rangoon", NULL, NULL },
    { "Myanmar Standard Time", "Asia/Yangon", NULL, NULL },
    { "SE Asia Standard Time", "Asia/Bangkok", NULL, NULL },
    { "SE Asia Standard Time", "Asia/Jakarta", "WIB", NULL },
    { "SE Asia Standard Time", "Asia/Ho_Chi_Minh", NULL, NULL },
    { "Altai Standard Time", "Asia/Barnaul", NULL, NULL },
    { "W. Mongolia Standard Time", "Asia/Hovd", NULL, NULL },
    { "North Asia Standard Time", "Asia/Krasnoyarsk", NULL, NULL },
    { "N. Central Asia Standard Time", "Asia/Novosibirsk", NULL, NULL },
    { "Tomsk Standard Time", "Asia/Tomsk", NULL, NULL },
    { "China Standard Time", "Asia/Shanghai", "CST", NULL },
    { "China Standard Time", "Asia/Hong_Kong", "HKT", NULL },
    { "North Asia East Standard Time", "Asia/Irkutsk", NULL, NULL },
    { "Singapore Standard Time", "Asia/Singapore", NULL, NULL },
    { "Singapore Standard Time", "Asia/Kuala_Lumpur", NULL, NULL },
    { "Singapore Standard Time", "Asia/Manila", "PST", NULL },
    { "W. Australia Standard Time", "Australia/Perth", "AWST", NULL },
    { "Taipei Standard Time", "Asia/Taipei", "CST", NULL },
    { "Ulaanbaatar Standard Time", "Asia/Ulaanbaatar", NULL, NULL },
    { "Aus Central W. Standard Time", "Australia/Eucla", NULL, NULL },
    { "Transbaikal Standard Time", "Asia/Chita", NULL, NULL },
    { "Tokyo Standard Time", "Asia/Tokyo", "JST", NULL },
    { "North Korea Standard Time", "Asia/Pyongyang", "KST", NULL },
    { "Korea Standard Time", "Asia/Seoul", "KST", NULL },
    { "Yakutsk Standard Time", "Asia/Yakutsk", NULL, NULL },
    { "Cen. Australia Standard Time", "Australia/Adelaide", "ACST", "ACDT" },
    { "AUS Central Standard Time", "Australia/Darwin", "ACST", NULL },
    { "E. Australia Standard Time", "Australia/Brisbane", "AEST", NULL },
    { "AUS Eastern Standard Time", "Australia/Sydney", "AEST", "AEDT" },
    { "AUS Eastern Standard Time", "Australia/Melbourne", "AEST", "AEDT" },
    { "West Pacific Standard Time", "Pacific/Port_Moresby", NULL, NULL },
    { "Tasmania Standard Time", "Australia/Hobart", "AEST", "AEDT" },
    { "Vladivostok Standard Time", "Asia/Vladivostok", NULL, NULL },
    { "Lord Howe Standard Time", "Australia/Lord_Howe", NULL, NULL },
    { "Bougainville Standard Time", "Pacific/Bougainville", NULL, NULL },
    { "Russia Time Zone 10", "Asia/Srednekolymsk", NULL, NULL },
    { "Magadan Standard Time", "Asia/Magadan", NULL, NULL },
    { "Norfolk Standard Time", "Pacific/Norfolk", NULL, NULL },
    { "Sakhalin Standard Time", "Asia/Sakhalin", NULL, NULL },
    { "Central Pacific Standard Time", "Pacific/Guadalcanal", NULL, NULL },
    { "Russia Time Zone 11", "Asia/Kamchatka", NULL, NULL },
    { "New Zealand Standard Time", "Pacific/Auckland", "NZST", "NZDT" },
    { "UTC+12", "Etc/GMT-12", NULL, NULL },
    { "Fiji Standard Time", "Pacific/Fiji", NULL, NULL },
    { "Chatham Islands Standard Time", "Pacific/Chatham", NULL, NULL },
    { "UTC+13", "Etc/GMT-13", NULL, NULL },
    { "Tonga Standard Time", "Pacific/Tongatapu", NULL, NULL },
    { "Samoa Standard Time", "Pacific/Apia", NULL, NULL },
    { "Line Islands Standard Time", "Pacific/Kiritimati", NULL, NULL },
};

#define NMAP (sizeof g_map / sizeof g_map[0])

/* REG_TZI_FORMAT */
typedef struct reg_tzi {
    LONG Bias, StandardBias, DaylightBias;
    SYSTEMTIME StandardDate, DaylightDate;
} reg_tzi;

/* ---- the TZif writer ---- */

typedef struct tz_type {
    int32_t off;
    int dst;
    char abbr[16];
} tz_type;

typedef struct tz_build {
    tz_type types[32];
    int ntypes;
    int64_t *trans;
    unsigned char *idx;
    int ntrans, cap;
} tz_build;

static void numeric_abbr(int32_t off, char *buf, size_t cap) {
    int32_t a = off < 0 ? -off : off;
    if (a % 3600 == 0) snprintf(buf, cap, "%c%02d", off < 0 ? '-' : '+', (int)(a / 3600));
    else snprintf(buf, cap, "%c%02d%02d", off < 0 ? '-' : '+', (int)(a / 3600), (int)(a % 3600 / 60));
}

static int type_of(tz_build *b, int32_t off, int dst, const char *abbr) {
    char ab[16];
    if (abbr) snprintf(ab, sizeof ab, "%s", abbr);
    else numeric_abbr(off, ab, sizeof ab);
    for (int i = 0; i < b->ntypes; i++)
        if (b->types[i].off == off && b->types[i].dst == dst && strcmp(b->types[i].abbr, ab) == 0) return i;
    if (b->ntypes == 32) return 0;
    tz_type *t = &b->types[b->ntypes];
    t->off = off;
    t->dst = dst;
    memcpy(t->abbr, ab, sizeof ab);
    return b->ntypes++;
}

static void add_trans(tz_build *b, int64_t at, int type) {
    if (b->ntrans > 0 && (b->idx[b->ntrans - 1] == type || b->trans[b->ntrans - 1] >= at)) {
        if (b->trans[b->ntrans - 1] >= at) b->idx[b->ntrans - 1] = (unsigned char)type;  /* same instant: last wins */
        return;
    }
    if (b->ntrans == 0 && type == 0) return;  /* type 0 is in effect before the first transition */
    if (b->ntrans == b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        int64_t *nt = realloc(b->trans, sizeof *nt * (size_t)cap);
        if (!nt) return;
        b->trans = nt;
        unsigned char *ni = realloc(b->idx, (size_t)cap);
        if (!ni) return;
        b->idx = ni;
        b->cap = cap;
    }
    b->trans[b->ntrans] = at;
    b->idx[b->ntrans++] = (unsigned char)type;
}

/* Days from 1970-01-01 of a civil date (proleptic Gregorian). */
static int64_t days_from_civil(int64_t y, int m, int d) {
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int days_in_month(int y, int m) {
    static const int dm[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (m == 2 && ((y % 4 == 0 && y % 100 != 0) || y % 400 == 0)) return 29;
    return dm[m - 1];
}

/* Seconds of a SYSTEMTIME's time of day (999 ms rounds up: Windows writes
 * "end of the day" as 23:59:59.999). */
static int64_t st_secs(const SYSTEMTIME *s) {
    return (int64_t)s->wHour * 3600 + s->wMinute * 60 + s->wSecond + (s->wMilliseconds >= 500 ? 1 : 0);
}

/* Local seconds since the epoch of a transition rule in `year`. */
static int64_t rule_local(const SYSTEMTIME *s, int year) {
    if (s->wYear) return days_from_civil(s->wYear, s->wMonth, s->wDay) * 86400 + st_secs(s);
    int64_t first = days_from_civil(year, s->wMonth, 1);
    int wd = (int)(((first + 4) % 7 + 7) % 7);  /* 1970-01-01 was a Thursday */
    int dom = 1 + ((s->wDayOfWeek - wd) % 7 + 7) % 7 + 7 * (s->wDay - 1);
    int dim = days_in_month(year, s->wMonth);
    while (dom > dim) dom -= 7;
    return (first + dom - 1) * 86400 + st_secs(s);
}

static bool has_dst(const reg_tzi *t) { return t->DaylightDate.wMonth != 0 && t->DaylightBias != 0; }

/* Transitions of one year's rule. */
static void year_trans(tz_build *b, const reg_tzi *t, int year, const tz_map *m) {
    int32_t so = -(t->Bias + t->StandardBias) * 60, doff = -(t->Bias + t->DaylightBias) * 60;
    int st = type_of(b, so, 0, m ? m->std : NULL);
    if (!has_dst(t)) {
        add_trans(b, days_from_civil(year, 1, 1) * 86400 - so, st);
        return;
    }
    int dt = type_of(b, doff, 1, m ? m->dst : NULL);
    int64_t start = rule_local(&t->DaylightDate, year) - so;  /* local standard time */
    int64_t end = rule_local(&t->StandardDate, year) - doff;  /* local daylight time */
    if (start < end) {
        add_trans(b, start, dt);
        add_trans(b, end, st);
    } else {
        add_trans(b, end, st);
        add_trans(b, start, dt);
    }
}

/* POSIX TZ "Mm.w.d/time" of a rule. */
static int rule_text(char *p, size_t cap, const SYSTEMTIME *s) {
    int64_t secs = st_secs(s);
    if (secs % 3600 == 0) return snprintf(p, cap, "M%d.%d.%d/%lld", s->wMonth, s->wDay, s->wDayOfWeek, (long long)(secs / 3600));
    return snprintf(p, cap, "M%d.%d.%d/%lld:%02lld:%02lld", s->wMonth, s->wDay, s->wDayOfWeek, (long long)(secs / 3600),
                    (long long)(secs % 3600 / 60), (long long)(secs % 60));
}

static int posix_name(char *p, size_t cap, const char *abbr) {
    bool alpha = strlen(abbr) >= 3;
    for (const char *c = abbr; *c; c++)
        if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z'))) alpha = false;
    return snprintf(p, cap, alpha ? "%s" : "<%s>", abbr);
}

static int posix_off(char *p, size_t cap, int32_t east) {
    int32_t w = -east, a = w < 0 ? -w : w;
    const char *sign = w < 0 ? "-" : "";
    if (a % 3600 == 0) return snprintf(p, cap, "%s%d", sign, (int)(a / 3600));
    if (a % 60 == 0) return snprintf(p, cap, "%s%d:%02d", sign, (int)(a / 3600), (int)(a % 3600 / 60));
    return snprintf(p, cap, "%s%d:%02d:%02d", sign, (int)(a / 3600), (int)(a % 3600 / 60), (int)(a % 60));
}

static void footer(char *buf, size_t cap, tz_build *b, const reg_tzi *t, const tz_map *m) {
    int32_t so = -(t->Bias + t->StandardBias) * 60, doff = -(t->Bias + t->DaylightBias) * 60;
    size_t k = 0;
    int st = type_of(b, so, 0, m ? m->std : NULL);
    k += (size_t)posix_name(buf + k, cap - k, b->types[st].abbr);
    k += (size_t)posix_off(buf + k, cap - k, so);
    if (has_dst(t) && !t->DaylightDate.wYear && !t->StandardDate.wYear) {
        int dt = type_of(b, doff, 1, m ? m->dst : NULL);
        k += (size_t)posix_name(buf + k, cap - k, b->types[dt].abbr);
        if (doff != so + 3600) k += (size_t)posix_off(buf + k, cap - k, doff);
        k += (size_t)snprintf(buf + k, cap - k, ",");
        k += (size_t)rule_text(buf + k, cap - k, &t->DaylightDate);
        k += (size_t)snprintf(buf + k, cap - k, ",");
        k += (size_t)rule_text(buf + k, cap - k, &t->StandardDate);
    }
    (void)k;
}

static void be32(unsigned char *p, int64_t v) {
    uint32_t u = (uint32_t)v;
    p[0] = (unsigned char)(u >> 24);
    p[1] = (unsigned char)(u >> 16);
    p[2] = (unsigned char)(u >> 8);
    p[3] = (unsigned char)u;
}

/* Serializes b (v1 body empty but valid, v2 body, footer). */
static bool tzif_bytes(tz_build *b, const char *foot, char **out, size_t *len) {
    char abbrs[32 * 16];
    size_t na = 0;
    unsigned char ai[32];
    for (int i = 0; i < b->ntypes; i++) {
        ai[i] = (unsigned char)na;
        size_t l = strlen(b->types[i].abbr) + 1;
        memcpy(abbrs + na, b->types[i].abbr, l);
        na += l;
    }
    size_t v1 = 44 + 6 + na;  /* one type (the first), no transitions */
    size_t v2 = 44 + (size_t)b->ntrans * 9 + (size_t)b->ntypes * 6 + na;
    size_t fl = strlen(foot);
    size_t total = v1 + v2 + fl + 2;
    unsigned char *p = calloc(1, total);
    if (!p) return false;
    unsigned char *q = p;
    /* v1 */
    memcpy(q, "TZif2", 5);
    be32(q + 36, 1);
    be32(q + 40, (int64_t)na);
    q += 44;
    be32(q, b->types[0].off);
    q[4] = (unsigned char)b->types[0].dst;
    q[5] = 0;
    q += 6;
    memcpy(q, abbrs, na);
    q += na;
    /* v2 */
    memcpy(q, "TZif2", 5);
    be32(q + 32, b->ntrans);
    be32(q + 36, b->ntypes);
    be32(q + 40, (int64_t)na);
    q += 44;
    for (int i = 0; i < b->ntrans; i++) {
        be32(q, b->trans[i] >> 32);
        be32(q + 4, b->trans[i] & 0xffffffff);
        q += 8;
    }
    for (int i = 0; i < b->ntrans; i++) *q++ = b->idx[i];
    for (int i = 0; i < b->ntypes; i++) {
        be32(q, b->types[i].off);
        q[4] = (unsigned char)b->types[i].dst;
        q[5] = ai[i];
        q += 6;
    }
    memcpy(q, abbrs, na);
    q += na;
    *q++ = '\n';
    memcpy(q, foot, fl);
    q += fl;
    *q++ = '\n';
    *out = (char *)p;
    *len = (size_t)(q - p);
    return true;
}

/* ---- registry ---- */

static HKEY open_zone(const char *key) {
    wchar_t *w = slang__w32_wide(key);
    if (!w) return NULL;
    wchar_t path[512];
    _snwprintf(path, 511, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones\\%ls", w);
    path[511] = 0;
    free(w);
    HKEY h = NULL;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, path, 0, KEY_READ, &h) != ERROR_SUCCESS) return NULL;
    return h;
}

static bool read_tzi(HKEY h, const wchar_t *sub, const wchar_t *value, reg_tzi *t) {
    DWORD sz = sizeof *t, type = 0;
    return RegGetValueW(h, sub, value, RRF_RT_REG_BINARY, &type, t, &sz) == ERROR_SUCCESS && sz == sizeof *t;
}

static bool read_dword(HKEY h, const wchar_t *sub, const wchar_t *value, DWORD *v) {
    DWORD sz = sizeof *v;
    return RegGetValueW(h, sub, value, RRF_RT_REG_DWORD, NULL, v, &sz) == ERROR_SUCCESS;
}

/* TZif for a registry key (m: the table entry for abbreviations, or NULL);
 * no_dst: standard time all year. */
static bool zone_tzif(const char *key, const tz_map *m, bool no_dst, const reg_tzi *fallback, char **out, size_t *len) {
    tz_build b;
    memset(&b, 0, sizeof b);
    reg_tzi cur;
    HKEY h = open_zone(key);
    bool have = h && read_tzi(h, NULL, L"TZI", &cur);
    if (!have) {
        if (!fallback) {
            if (h) RegCloseKey(h);
            return false;
        }
        cur = *fallback;
    }
    if (no_dst) cur.DaylightDate.wMonth = 0;
    /* type 0: the standard offset of the first rule known */
    DWORD first = 0, last = 0;
    bool dyn = !no_dst && h && read_dword(h, L"Dynamic DST", L"FirstEntry", &first) &&
               read_dword(h, L"Dynamic DST", L"LastEntry", &last) && first <= last && last - first < 400;
    reg_tzi r0 = cur;
    if (dyn) {
        wchar_t y[16];
        _snwprintf(y, 15, L"%lu", (unsigned long)first);
        y[15] = 0;
        if (!read_tzi(h, L"Dynamic DST", y, &r0)) r0 = cur;
    }
    type_of(&b, -(r0.Bias + r0.StandardBias) * 60, 0, m ? m->std : NULL);
    if (dyn) {
        for (DWORD year = first; year <= last; year++) {
            wchar_t y[16];
            _snwprintf(y, 15, L"%lu", (unsigned long)year);
            y[15] = 0;
            reg_tzi t;
            if (!read_tzi(h, L"Dynamic DST", y, &t)) t = cur;
            year_trans(&b, &t, (int)year, m);
        }
    }
    if (h) RegCloseKey(h);
    char foot[160];
    footer(foot, sizeof foot, &b, &cur, m);
    bool ok = tzif_bytes(&b, foot, out, len);
    free(b.trans);
    free(b.idx);
    return ok;
}

static const tz_map *by_key(const char *key) {
    for (size_t i = 0; i < NMAP; i++)
        if (strcmp(g_map[i].win, key) == 0) return &g_map[i];
    return NULL;
}

bool slang__plat_tz_tzif(const char *name, char **out, size_t *len) {
    if (name) {
        for (size_t i = 0; i < NMAP; i++)
            if (strcmp(g_map[i].iana, name) == 0) return zone_tzif(g_map[i].win, &g_map[i], false, NULL, out, len);
        return false;
    }
    DYNAMIC_TIME_ZONE_INFORMATION d;
    memset(&d, 0, sizeof d);
    if (GetDynamicTimeZoneInformation(&d) == TIME_ZONE_ID_INVALID) return false;
    char *key = slang__w32_utf8(d.TimeZoneKeyName, -1);
    reg_tzi fb;
    fb.Bias = d.Bias;
    fb.StandardBias = d.StandardBias;
    fb.DaylightBias = d.DaylightBias;
    fb.StandardDate = d.StandardDate;
    fb.DaylightDate = d.DaylightDate;
    bool ok = zone_tzif(key ? key : "", key ? by_key(key) : NULL, d.DynamicDaylightTimeDisabled != 0, &fb, out, len);
    free(key);
    return ok;
}

#else
typedef int slang__tz_win32_unused;
#endif
