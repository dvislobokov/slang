/* std_sql.c - sentinel errors of std.database.sql (compiler/src/sema/
 * stdlib_src/sql.sl; runtime/ABI.md section 23). The pool, the driver
 * interface and the typed mapping are Slang. */
#include "rt_internal.h"

#define SQL_SENTINEL(fn, lit)                                                         \
    static struct { slang_object hdr; int64_t len; char data[sizeof(lit)]; } fn##_msg = \
        { { SLANG_RC_IMMORTAL, &slang_type_string }, (int64_t)(sizeof(lit) - 1), lit };  \
    static slang_error fn##_err = { .hdr = { SLANG_RC_IMMORTAL, &slang_type_error },     \
                                    .message = (slang_string *)&fn##_msg };              \
    slang_error *fn(void) { return &fn##_err; }

SQL_SENTINEL(slang_database_sql_err_no_rows, "sql: no rows in result set")
SQL_SENTINEL(slang_database_sql_err_tx_done, "sql: transaction has already been committed or rolled back")
SQL_SENTINEL(slang_database_sql_err_conn_done, "sql: database is closed")
