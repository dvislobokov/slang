/* std_sqlite.c - the C side of std.database.sqlite (compiler/src/sema/
 * stdlib_src/sqlite.sl; runtime/ABI.md section 23): a thin wrapper over the
 * system SQLite library (libsqlite3). Connections and statements are raw
 * `sqlite3 *` / `sqlite3_stmt *` pointers passed to Slang as int handles;
 * the Slang side owns them (Drop closes/finalizes).
 *
 * The compiler links `-lsqlite3` only into programs that call one of these
 * functions (stdlib.rs LINK_LIBS). When the runtime is built without
 * <sqlite3.h>, every function fails with "sqlite: not available" and
 * rtAvailable() is false. */
#include "rt_internal.h"

#include <stdio.h>
#include <string.h>

#if defined(__has_include)
#if __has_include(<sqlite3.h>)
#define SLANG_HAVE_SQLITE 1
#endif
#endif

static void sq_err(slang_error **err, const char *msg) {
    if (err) *err = slang_error_new_cstr(msg);
}

#ifdef SLANG_HAVE_SQLITE
#include <sqlite3.h>

#define DB(h) ((sqlite3 *)(intptr_t)(h))
#define ST(h) ((sqlite3_stmt *)(intptr_t)(h))

static void db_err(slang_error **err, sqlite3 *db, int rc) {
    if (!err) return;
    const char *m = db ? sqlite3_errmsg(db) : sqlite3_errstr(rc);
    *err = slang_error_new_cstr(m ? m : "unknown error");
}

bool slang_database_sqlite_rt_available(void) { return true; }

slang_string *slang_database_sqlite_rt_version(void) { return slang_string_from_cstr(sqlite3_libversion()); }

int64_t slang_database_sqlite_rt_open(const slang_string *path, int64_t flags, slang_error **err) {
    sqlite3 *db = NULL;
    int f = flags ? (int)flags : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    f |= SQLITE_OPEN_URI | SQLITE_OPEN_FULLMUTEX;
    int rc = sqlite3_open_v2(slang_string_data(path), &db, f, NULL);
    if (rc != SQLITE_OK) {
        db_err(err, db, rc);
        sqlite3_close_v2(db);
        return 0;
    }
    sqlite3_extended_result_codes(db, 1);
    sqlite3_busy_timeout(db, 5000);
    return (int64_t)(intptr_t)db;
}

void slang_database_sqlite_rt_close(int64_t h) {
    if (h) sqlite3_close_v2(DB(h));
}

void slang_database_sqlite_rt_busy_timeout(int64_t h, int64_t ms) {
    if (h) sqlite3_busy_timeout(DB(h), (int)ms);
}

void slang_database_sqlite_rt_exec(int64_t h, const slang_string *sql, slang_error **err) {
    int rc = sqlite3_exec(DB(h), slang_string_data(sql), NULL, NULL, NULL);
    if (rc != SQLITE_OK) db_err(err, DB(h), rc);
}

/* Prepares the first statement of sql. A trailing remainder that is not
 * only white space and semicolons is an error (use rtExec for scripts). */
int64_t slang_database_sqlite_rt_prepare(int64_t h, const slang_string *sql, slang_error **err) {
    sqlite3_stmt *st = NULL;
    const char *tail = NULL;
    const char *s = slang_string_data(sql);
    int rc = sqlite3_prepare_v2(DB(h), s, (int)slang_string_len(sql), &st, &tail);
    if (rc != SQLITE_OK) {
        db_err(err, DB(h), rc);
        return 0;
    }
    if (tail) {
        const char *end = s + slang_string_len(sql);
        while (tail < end && (*tail == ' ' || *tail == '\t' || *tail == '\n' || *tail == '\r' || *tail == ';')) tail++;
        if (tail < end) {
            sqlite3_finalize(st);
            sq_err(err, "sqlite: more than one statement (use Exec without arguments for scripts)");
            return 0;
        }
    }
    if (!st) {
        sq_err(err, "sqlite: empty statement");
        return 0;
    }
    return (int64_t)(intptr_t)st;
}

void slang_database_sqlite_rt_finalize(int64_t st) {
    if (st) sqlite3_finalize(ST(st));
}

void slang_database_sqlite_rt_reset(int64_t st) {
    sqlite3_reset(ST(st));
    sqlite3_clear_bindings(ST(st));
}

int64_t slang_database_sqlite_rt_param_count(int64_t st) { return sqlite3_bind_parameter_count(ST(st)); }

/* Index (1-based) of a named parameter (":x", "@x", "$x"), 0 if none. */
int64_t slang_database_sqlite_rt_param_index(int64_t st, const slang_string *name) {
    return sqlite3_bind_parameter_index(ST(st), slang_string_data(name));
}

static void bind_rc(int64_t st, int rc, slang_error **err) {
    if (rc != SQLITE_OK) db_err(err, sqlite3_db_handle(ST(st)), rc);
}

void slang_database_sqlite_rt_bind_null(int64_t st, int64_t i, slang_error **err) {
    bind_rc(st, sqlite3_bind_null(ST(st), (int)i), err);
}

void slang_database_sqlite_rt_bind_int(int64_t st, int64_t i, int64_t v, slang_error **err) {
    bind_rc(st, sqlite3_bind_int64(ST(st), (int)i, v), err);
}

void slang_database_sqlite_rt_bind_float(int64_t st, int64_t i, double v, slang_error **err) {
    bind_rc(st, sqlite3_bind_double(ST(st), (int)i, v), err);
}

void slang_database_sqlite_rt_bind_text(int64_t st, int64_t i, const slang_string *v, slang_error **err) {
    bind_rc(st, sqlite3_bind_text64(ST(st), (int)i, slang_string_data(v), (sqlite3_uint64)slang_string_len(v),
                                    SQLITE_TRANSIENT, SQLITE_UTF8), err);
}

void slang_database_sqlite_rt_bind_blob(int64_t st, int64_t i, const slang_slice *v, slang_error **err) {
    int64_t n = v ? v->len : 0;
    bind_rc(st, sqlite3_bind_blob64(ST(st), (int)i, n ? v->data : "", (sqlite3_uint64)n, SQLITE_TRANSIENT), err);
}

/* 1: a row is available, 0: done. */
int64_t slang_database_sqlite_rt_step(int64_t st, slang_error **err) {
    int rc = sqlite3_step(ST(st));
    if (rc == SQLITE_ROW) return 1;
    if (rc == SQLITE_DONE) return 0;
    db_err(err, sqlite3_db_handle(ST(st)), rc);
    return 0;
}

int64_t slang_database_sqlite_rt_column_count(int64_t st) { return sqlite3_column_count(ST(st)); }

slang_string *slang_database_sqlite_rt_column_name(int64_t st, int64_t i) {
    const char *n = sqlite3_column_name(ST(st), (int)i);
    return slang_string_from_cstr(n ? n : "");
}

slang_string *slang_database_sqlite_rt_column_decltype(int64_t st, int64_t i) {
    const char *n = sqlite3_column_decltype(ST(st), (int)i);
    return slang_string_from_cstr(n ? n : "");
}

/* 1 INTEGER, 2 FLOAT, 3 TEXT, 4 BLOB, 5 NULL (SQLite's codes). */
int64_t slang_database_sqlite_rt_column_type(int64_t st, int64_t i) { return sqlite3_column_type(ST(st), (int)i); }

int64_t slang_database_sqlite_rt_column_int(int64_t st, int64_t i) { return sqlite3_column_int64(ST(st), (int)i); }

double slang_database_sqlite_rt_column_float(int64_t st, int64_t i) { return sqlite3_column_double(ST(st), (int)i); }

slang_string *slang_database_sqlite_rt_column_text(int64_t st, int64_t i) {
    const unsigned char *t = sqlite3_column_text(ST(st), (int)i);
    int n = sqlite3_column_bytes(ST(st), (int)i);
    return slang_string_new(t ? (const char *)t : "", t ? n : 0);
}

void slang_database_sqlite_rt_column_blob(int64_t st, int64_t i, slang_slice *out) {
    const void *b = sqlite3_column_blob(ST(st), (int)i);
    int n = sqlite3_column_bytes(ST(st), (int)i);
    slang_slice_from(out, 1, NULL, b ? b : "", b ? n : 0);
}

int64_t slang_database_sqlite_rt_changes(int64_t h) { return sqlite3_changes64(DB(h)); }

int64_t slang_database_sqlite_rt_last_insert_id(int64_t h) { return sqlite3_last_insert_rowid(DB(h)); }

int64_t slang_database_sqlite_rt_errcode(int64_t h) { return h ? sqlite3_extended_errcode(DB(h)) : 0; }

bool slang_database_sqlite_rt_autocommit(int64_t h) { return sqlite3_get_autocommit(DB(h)) != 0; }

#else /* !SLANG_HAVE_SQLITE */

#define NA "sqlite: not available (the runtime was built without <sqlite3.h>)"

bool slang_database_sqlite_rt_available(void) { return false; }
slang_string *slang_database_sqlite_rt_version(void) { return slang_string_from_cstr(""); }
int64_t slang_database_sqlite_rt_open(const slang_string *path, int64_t flags, slang_error **err) {
    (void)path; (void)flags;
    sq_err(err, NA);
    return 0;
}
void slang_database_sqlite_rt_close(int64_t h) { (void)h; }
void slang_database_sqlite_rt_busy_timeout(int64_t h, int64_t ms) { (void)h; (void)ms; }
void slang_database_sqlite_rt_exec(int64_t h, const slang_string *sql, slang_error **err) {
    (void)h; (void)sql;
    sq_err(err, NA);
}
int64_t slang_database_sqlite_rt_prepare(int64_t h, const slang_string *sql, slang_error **err) {
    (void)h; (void)sql;
    sq_err(err, NA);
    return 0;
}
void slang_database_sqlite_rt_finalize(int64_t st) { (void)st; }
void slang_database_sqlite_rt_reset(int64_t st) { (void)st; }
int64_t slang_database_sqlite_rt_param_count(int64_t st) { (void)st; return 0; }
int64_t slang_database_sqlite_rt_param_index(int64_t st, const slang_string *name) { (void)st; (void)name; return 0; }
void slang_database_sqlite_rt_bind_null(int64_t st, int64_t i, slang_error **err) { (void)st; (void)i; sq_err(err, NA); }
void slang_database_sqlite_rt_bind_int(int64_t st, int64_t i, int64_t v, slang_error **err) { (void)st; (void)i; (void)v; sq_err(err, NA); }
void slang_database_sqlite_rt_bind_float(int64_t st, int64_t i, double v, slang_error **err) { (void)st; (void)i; (void)v; sq_err(err, NA); }
void slang_database_sqlite_rt_bind_text(int64_t st, int64_t i, const slang_string *v, slang_error **err) { (void)st; (void)i; (void)v; sq_err(err, NA); }
void slang_database_sqlite_rt_bind_blob(int64_t st, int64_t i, const slang_slice *v, slang_error **err) { (void)st; (void)i; (void)v; sq_err(err, NA); }
int64_t slang_database_sqlite_rt_step(int64_t st, slang_error **err) { (void)st; sq_err(err, NA); return 0; }
int64_t slang_database_sqlite_rt_column_count(int64_t st) { (void)st; return 0; }
slang_string *slang_database_sqlite_rt_column_name(int64_t st, int64_t i) { (void)st; (void)i; return slang_string_from_cstr(""); }
slang_string *slang_database_sqlite_rt_column_decltype(int64_t st, int64_t i) { (void)st; (void)i; return slang_string_from_cstr(""); }
int64_t slang_database_sqlite_rt_column_type(int64_t st, int64_t i) { (void)st; (void)i; return 5; }
int64_t slang_database_sqlite_rt_column_int(int64_t st, int64_t i) { (void)st; (void)i; return 0; }
double slang_database_sqlite_rt_column_float(int64_t st, int64_t i) { (void)st; (void)i; return 0; }
slang_string *slang_database_sqlite_rt_column_text(int64_t st, int64_t i) { (void)st; (void)i; return slang_string_from_cstr(""); }
void slang_database_sqlite_rt_column_blob(int64_t st, int64_t i, slang_slice *out) { (void)st; (void)i; memset(out, 0, sizeof *out); }
int64_t slang_database_sqlite_rt_changes(int64_t h) { (void)h; return 0; }
int64_t slang_database_sqlite_rt_last_insert_id(int64_t h) { (void)h; return 0; }
int64_t slang_database_sqlite_rt_errcode(int64_t h) { (void)h; return 0; }
bool slang_database_sqlite_rt_autocommit(int64_t h) { (void)h; return true; }

#endif
