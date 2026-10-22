/* sqlite_dyn.h - SQLite bound at run time from a system DLL, for
 * std_sqlite.c on systems whose C headers have no <sqlite3.h> but whose
 * OS ships the library: Windows 10+ has winsqlite3.dll in System32 (the
 * SQLite build Windows itself uses); a sqlite3.dll next to the program or
 * on PATH is tried first, so an application can bring a newer one.
 *
 * Included by std_sqlite.c after its <sqlite3.h> probe. When the probe
 * failed and the target is Windows, this header declares the subset of the
 * SQLite C API that std_sqlite.c uses as function pointers (same names via
 * macros), defines ZELL_HAVE_SQLITE and ZELL_SQLITE_DYNAMIC, and
 * zell__sqlite_load() binds them (false: no DLL, rtAvailable() is false
 * and Open fails with "sqlite: not available"). Elsewhere it does nothing:
 * Linux/macOS link -lsqlite3 (stdlib.rs LINK_LIBS) when the headers exist.
 * The compiler leaves -lsqlite3 out when linking for Windows
 * (crosslink.rs). */
#ifndef ZELL_SQLITE_DYN_H
#define ZELL_SQLITE_DYN_H

#if !defined(ZELL_HAVE_SQLITE) && defined(_WIN32)

#include <stdbool.h>
#include <stdint.h>

#define ZELL_HAVE_SQLITE 1
#define ZELL_SQLITE_DYNAMIC 1

typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
typedef long long sqlite3_int64;
typedef unsigned long long sqlite3_uint64;

#define SQLITE_OK 0
#define SQLITE_ROW 100
#define SQLITE_DONE 101
#define SQLITE_UTF8 1
#define SQLITE_OPEN_READWRITE 0x00000002
#define SQLITE_OPEN_CREATE 0x00000004
#define SQLITE_OPEN_URI 0x00000040
#define SQLITE_OPEN_FULLMUTEX 0x00010000
#define SQLITE_TRANSIENT ((void (*)(void *)) - 1)

/* ret, name, parameters */
#define ZELL_SQLITE_API(X)                                                                          \
    X(const char *, sqlite3_libversion, (void))                                                      \
    X(const char *, sqlite3_errmsg, (sqlite3 *))                                                     \
    X(const char *, sqlite3_errstr, (int))                                                           \
    X(int, sqlite3_open_v2, (const char *, sqlite3 **, int, const char *))                           \
    X(int, sqlite3_close_v2, (sqlite3 *))                                                            \
    X(int, sqlite3_extended_result_codes, (sqlite3 *, int))                                          \
    X(int, sqlite3_busy_timeout, (sqlite3 *, int))                                                   \
    X(int, sqlite3_exec, (sqlite3 *, const char *, int (*)(void *, int, char **, char **), void *, char **)) \
    X(int, sqlite3_prepare_v2, (sqlite3 *, const char *, int, sqlite3_stmt **, const char **))       \
    X(int, sqlite3_finalize, (sqlite3_stmt *))                                                       \
    X(int, sqlite3_reset, (sqlite3_stmt *))                                                          \
    X(int, sqlite3_clear_bindings, (sqlite3_stmt *))                                                 \
    X(int, sqlite3_bind_parameter_count, (sqlite3_stmt *))                                           \
    X(int, sqlite3_bind_parameter_index, (sqlite3_stmt *, const char *))                             \
    X(int, sqlite3_bind_null, (sqlite3_stmt *, int))                                                 \
    X(int, sqlite3_bind_int64, (sqlite3_stmt *, int, sqlite3_int64))                                 \
    X(int, sqlite3_bind_double, (sqlite3_stmt *, int, double))                                       \
    X(int, sqlite3_bind_text64, (sqlite3_stmt *, int, const char *, sqlite3_uint64, void (*)(void *), unsigned char)) \
    X(int, sqlite3_bind_blob64, (sqlite3_stmt *, int, const void *, sqlite3_uint64, void (*)(void *))) \
    X(int, sqlite3_step, (sqlite3_stmt *))                                                           \
    X(sqlite3 *, sqlite3_db_handle, (sqlite3_stmt *))                                                \
    X(int, sqlite3_column_count, (sqlite3_stmt *))                                                   \
    X(const char *, sqlite3_column_name, (sqlite3_stmt *, int))                                      \
    X(const char *, sqlite3_column_decltype, (sqlite3_stmt *, int))                                  \
    X(int, sqlite3_column_type, (sqlite3_stmt *, int))                                               \
    X(sqlite3_int64, sqlite3_column_int64, (sqlite3_stmt *, int))                                    \
    X(double, sqlite3_column_double, (sqlite3_stmt *, int))                                          \
    X(const unsigned char *, sqlite3_column_text, (sqlite3_stmt *, int))                             \
    X(const void *, sqlite3_column_blob, (sqlite3_stmt *, int))                                      \
    X(int, sqlite3_column_bytes, (sqlite3_stmt *, int))                                              \
    X(int, sqlite3_changes, (sqlite3 *))                                                             \
    X(sqlite3_int64, sqlite3_last_insert_rowid, (sqlite3 *))                                         \
    X(int, sqlite3_extended_errcode, (sqlite3 *))                                                    \
    X(int, sqlite3_get_autocommit, (sqlite3 *))

typedef struct zell__sqlite_fns {
#define ZELL_SQLITE_FIELD(ret, name, args) ret(*name) args;
    ZELL_SQLITE_API(ZELL_SQLITE_FIELD)
#undef ZELL_SQLITE_FIELD
    sqlite3_int64 (*sqlite3_changes64)(sqlite3 *);  /* 3.37+, else sqlite3_changes */
} zell__sqlite_fns;

extern zell__sqlite_fns zell__sqlite;
/* Binds the API once; true if the library is there. */
bool zell__sqlite_load(void);

#if !defined(ZELL_SQLITE_NO_MACROS)
#define sqlite3_libversion zell__sqlite.sqlite3_libversion
#define sqlite3_errmsg zell__sqlite.sqlite3_errmsg
#define sqlite3_errstr zell__sqlite.sqlite3_errstr
#define sqlite3_open_v2 zell__sqlite.sqlite3_open_v2
#define sqlite3_close_v2 zell__sqlite.sqlite3_close_v2
#define sqlite3_extended_result_codes zell__sqlite.sqlite3_extended_result_codes
#define sqlite3_busy_timeout zell__sqlite.sqlite3_busy_timeout
#define sqlite3_exec zell__sqlite.sqlite3_exec
#define sqlite3_prepare_v2 zell__sqlite.sqlite3_prepare_v2
#define sqlite3_finalize zell__sqlite.sqlite3_finalize
#define sqlite3_reset zell__sqlite.sqlite3_reset
#define sqlite3_clear_bindings zell__sqlite.sqlite3_clear_bindings
#define sqlite3_bind_parameter_count zell__sqlite.sqlite3_bind_parameter_count
#define sqlite3_bind_parameter_index zell__sqlite.sqlite3_bind_parameter_index
#define sqlite3_bind_null zell__sqlite.sqlite3_bind_null
#define sqlite3_bind_int64 zell__sqlite.sqlite3_bind_int64
#define sqlite3_bind_double zell__sqlite.sqlite3_bind_double
#define sqlite3_bind_text64 zell__sqlite.sqlite3_bind_text64
#define sqlite3_bind_blob64 zell__sqlite.sqlite3_bind_blob64
#define sqlite3_step zell__sqlite.sqlite3_step
#define sqlite3_db_handle zell__sqlite.sqlite3_db_handle
#define sqlite3_column_count zell__sqlite.sqlite3_column_count
#define sqlite3_column_name zell__sqlite.sqlite3_column_name
#define sqlite3_column_decltype zell__sqlite.sqlite3_column_decltype
#define sqlite3_column_type zell__sqlite.sqlite3_column_type
#define sqlite3_column_int64 zell__sqlite.sqlite3_column_int64
#define sqlite3_column_double zell__sqlite.sqlite3_column_double
#define sqlite3_column_text zell__sqlite.sqlite3_column_text
#define sqlite3_column_blob zell__sqlite.sqlite3_column_blob
#define sqlite3_column_bytes zell__sqlite.sqlite3_column_bytes
#define sqlite3_changes zell__sqlite.sqlite3_changes
#define sqlite3_changes64 zell__sqlite.sqlite3_changes64
#define sqlite3_last_insert_rowid zell__sqlite.sqlite3_last_insert_rowid
#define sqlite3_extended_errcode zell__sqlite.sqlite3_extended_errcode
#define sqlite3_get_autocommit zell__sqlite.sqlite3_get_autocommit
#endif

#endif
#endif
