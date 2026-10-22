/* std_sql.c - sentinel errors of database.sql (compiler/src/sema/
 * stdlib_src/sql.zl; runtime/ABI.md section 23). The pool, the driver
 * interface and the typed mapping are Zell. */
#include "rt_internal.h"

#define SQL_SENTINEL(fn, lit)                                                         \
    static ZELL_STRING_STORAGE(sizeof(lit)) fn##_msg = ZELL_STRING_INIT(fn##_msg, lit); \
    static zell_error fn##_err = { .hdr = { ZELL_RC_IMMORTAL, &zell_type_error },     \
                                    .message = (zell_string *)&fn##_msg };              \
    zell_error *fn(void) { return &fn##_err; }

SQL_SENTINEL(zell_database_sql_err_no_rows, "sql: no rows in result set")
SQL_SENTINEL(zell_database_sql_err_tx_done, "sql: transaction has already been committed or rolled back")
SQL_SENTINEL(zell_database_sql_err_conn_done, "sql: database is closed")
