/* std_time_zone.c - the local time zone for std.time (time.sl): the TZif
 * bytes of $TZ (":Name", "Name" or an absolute path; empty = UTC) or else
 * /etc/localtime, read once per process and cached (like Go, later
 * changes of TZ are not seen). An unreadable zone gives "" (UTC). Where
 * the system keeps zones elsewhere (Windows: the registry) the platform
 * layer synthesizes TZif (slang__plat_tz_tzif): for the local zone, for
 * a $TZ name without $ZONEINFO, and for time.LoadLocation
 * (slang_time_rt_zone_tzif). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

slang_string *slang_time_rt_local_tzif(slang_error **err);
slang_string *slang_time_rt_zone_tzif(const slang_string *name, slang_error **err);

static slang__plat_once once = SLANG_PLAT_ONCE_INIT;
static char *cached;
static size_t cached_len;

static void read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char buf[65536];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n == 0 || n == sizeof buf) return;
    cached = malloc(n);
    if (!cached) return;
    memcpy(cached, buf, n);
    cached_len = n;
}

static void load(void) {
    const char *tz = getenv("TZ");
    if (!tz) {
        const char *lt = slang__plat_localtime_path();
        if (lt) read_file(lt);
        else if (!slang__plat_tz_tzif(NULL, &cached, &cached_len)) cached = NULL, cached_len = 0;
        return;
    }
    if (*tz == ':') tz++;
    if (*tz == 0 || strcmp(tz, "UTC") == 0) return;
    if (*tz == '/' || (tz[0] && tz[1] == ':')) {  /* absolute (also C:\\...) */
        read_file(tz);
        return;
    }
    if (strstr(tz, "..")) return;
    const char *dir = getenv("ZONEINFO");
    if (!dir || !*dir) dir = slang__plat_zoneinfo_dir();
    if (!dir) {
        if (!slang__plat_tz_tzif(tz, &cached, &cached_len)) cached = NULL, cached_len = 0;
        return;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, tz);
    read_file(path);
}

slang_string *slang_time_rt_local_tzif(slang_error **err) {
    if (err) *err = NULL;
    slang__plat_once_run(&once, load);
    return slang_string_new(cached ? cached : "", (int64_t)cached_len);
}

/* TZif of an IANA name from the platform's own zone database (Windows);
 * "" where zones are tzdata files (time.sl reads those itself). */
slang_string *slang_time_rt_zone_tzif(const slang_string *name, slang_error **err) {
    if (err) *err = NULL;
    char *data = NULL;
    size_t len = 0;
    if (!slang__plat_tz_tzif(slang_string_cstr(name), &data, &len)) return slang_string_new("", 0);
    slang_string *s = slang_string_new(data, (int64_t)len);
    free(data);
    return s;
}
