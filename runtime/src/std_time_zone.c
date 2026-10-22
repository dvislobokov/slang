/* std_time_zone.c - the local time zone for time (time.zl): the TZif
 * bytes of $TZ (":Name", "Name" or an absolute path; empty = UTC) or else
 * /etc/localtime, read once per process and cached (like Go, later
 * changes of TZ are not seen). An unreadable zone gives "" (UTC). Where
 * the system keeps zones elsewhere (Windows: the registry) the platform
 * layer synthesizes TZif (zell__plat_tz_tzif): for the local zone, for
 * a $TZ name without $ZONEINFO, and for time.LoadLocation
 * (zell_time_rt_zone_tzif). */
#include "rt_internal.h"
#include "platform/platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

zell_string *zell_time_rt_local_tzif(zell_error **err);
zell_string *zell_time_rt_zone_tzif(const zell_string *name, zell_error **err);

static zell__plat_once once = ZELL_PLAT_ONCE_INIT;
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
        const char *lt = zell__plat_localtime_path();
        if (lt) read_file(lt);
        else if (!zell__plat_tz_tzif(NULL, &cached, &cached_len)) cached = NULL, cached_len = 0;
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
    if (!dir || !*dir) dir = zell__plat_zoneinfo_dir();
    if (!dir) {
        if (!zell__plat_tz_tzif(tz, &cached, &cached_len)) cached = NULL, cached_len = 0;
        return;
    }
    char path[4096];
    snprintf(path, sizeof path, "%s/%s", dir, tz);
    read_file(path);
}

zell_string *zell_time_rt_local_tzif(zell_error **err) {
    if (err) *err = NULL;
    zell__plat_once_run(&once, load);
    return zell_string_new(cached ? cached : "", (int64_t)cached_len);
}

/* TZif of an IANA name from the platform's own zone database (Windows);
 * "" where zones are tzdata files (time.zl reads those itself). */
zell_string *zell_time_rt_zone_tzif(const zell_string *name, zell_error **err) {
    if (err) *err = NULL;
    char *data = NULL;
    size_t len = 0;
    if (!zell__plat_tz_tzif(zell_string_cstr(name), &data, &len)) return zell_string_new("", 0);
    zell_string *s = zell_string_new(data, (int64_t)len);
    free(data);
    return s;
}
