/*
 * Limceron Runtime — PostgreSQL Driver (binding over libpq)
 * Provides: connect, query, execute, close, escape
 * C99, requires libpq (-lpq).
 *
 * Dual-path:
 *   - Compiled with -DLCN_HAS_POSTGRES: real database via libpq
 *   - Without: stub returning mock results (always compiles)
 */
#ifndef LCN_POSTGRES_DRIVER_H
#define LCN_POSTGRES_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* Opaque connection handle */
typedef struct LcnPgConn LcnPgConn;

/* Query result — holds rows as string arrays */
typedef struct {
    char   ***rows;         /* rows[row_index][col_index] = string value */
    char   **col_names;     /* column names */
    int      row_count;
    int      col_count;
    int      _capacity;
} LcnPgResult;

/* Connect to PostgreSQL. Returns NULL on allocation failure.
 * Check lcn_pg_error() for connection errors. */
LcnPgConn *lcn_pg_connect(const char *host, int port,
                           const char *user, const char *password,
                           const char *database);

/* Execute a SELECT query. Returns result set. Caller must free with lcn_pg_result_free(). */
LcnPgResult *lcn_pg_query(LcnPgConn *conn, const char *sql);

/* Execute an INSERT/UPDATE/DELETE. Returns affected row count, or -1 on error. */
int64_t lcn_pg_execute(LcnPgConn *conn, const char *sql);

/* Get string value from result. Returns "" if NULL or out of bounds. */
const char *lcn_pg_row_get(LcnPgResult *result, int row, const char *col_name);

/* Get numeric value from result. Returns 0 if NULL or out of bounds. */
int64_t lcn_pg_row_get_number(LcnPgResult *result, int row, const char *col_name);

/* Get row count from result. */
int lcn_pg_row_count(LcnPgResult *result);

/* Free a result set. */
void lcn_pg_result_free(LcnPgResult *result);

/* Close connection and free resources. */
void lcn_pg_close(LcnPgConn *conn);

/* Get last error message. */
const char *lcn_pg_error(LcnPgConn *conn);

/* Escape a string literal for safe SQL interpolation.
 * Returns a malloc'd string that the caller must free. */
char *lcn_pg_escape(LcnPgConn *conn, const char *str);

#endif /* LCN_POSTGRES_DRIVER_H */
