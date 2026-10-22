/* sqlite_win32.c - binds SQLite from sqlite3.dll / winsqlite3.dll at run
 * time for std_sqlite.c (see sqlite_dyn.h). */
#include "platform.h"

#if defined(_WIN32) && !(defined(__has_include) && __has_include(<sqlite3.h>))

#define ZELL_SQLITE_NO_MACROS 1
#include "sqlite_dyn.h"
#include "win32_util.h"

zell__sqlite_fns zell__sqlite;
static zell__plat_once g_once = ZELL_PLAT_ONCE_INIT;
static bool g_ok;

static sqlite3_int64 changes_compat(sqlite3 *db) { return zell__sqlite.sqlite3_changes(db); }

static void load(void) {
    /* an application's own sqlite3.dll (next to the .exe or on PATH) first */
    HMODULE h = LoadLibraryExW(L"sqlite3.dll", NULL, 0);
    if (!h) h = LoadLibraryExW(L"winsqlite3.dll", NULL, 0x00000800 /* LOAD_LIBRARY_SEARCH_SYSTEM32 */);
    if (!h) h = LoadLibraryW(L"winsqlite3.dll");
    if (!h) return;
    bool ok = true;
#define ZELL_SQLITE_BIND(ret, name, args)                                \
    zell__sqlite.name = (ret(*) args)(void *)GetProcAddress(h, #name); \
    if (!zell__sqlite.name) ok = false;
    ZELL_SQLITE_API(ZELL_SQLITE_BIND)
#undef ZELL_SQLITE_BIND
    zell__sqlite.sqlite3_changes64 = (sqlite3_int64(*)(sqlite3 *))(void *)GetProcAddress(h, "sqlite3_changes64");
    if (!zell__sqlite.sqlite3_changes64) zell__sqlite.sqlite3_changes64 = changes_compat;
    g_ok = ok;
}

bool zell__sqlite_load(void) {
    zell__plat_once_run(&g_once, load);
    return g_ok;
}

#else
typedef int zell__sqlite_win32_unused;
#endif
