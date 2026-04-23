/*
 * Limceron Runtime — MySQL Driver (binding over libmysqlclient)
 * Provides: connect, query, execute, close
 * C99, requires libmysqlclient (-lmysqlclient).
 */
#ifndef LCN_MYSQL_DRIVER_H
#define LCN_MYSQL_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* Opaque connection handle */
typedef struct LcnDbConn LcnDbConn;

/* Query result — holds rows as string arrays */
typedef struct {
    char   ***rows;         /* rows[row_index][col_index] = string value */
    char   **col_names;     /* column names */
    int      row_count;
    int      col_count;
    int      _capacity;
} LcnDbResult;

/* Connect to MySQL. Returns NULL on error. */
LcnDbConn *lcn_db_connect(const char *host, const char *user,
                           const char *password, const char *database,
                           int port);

/* Execute a SELECT query. Returns result set. Caller must free with lcn_db_result_free(). */
LcnDbResult *lcn_db_query(LcnDbConn *conn, const char *sql);

/* Execute an INSERT/UPDATE/DELETE. Returns affected row count, or -1 on error. */
int64_t lcn_db_execute(LcnDbConn *conn, const char *sql);

/* Get string value from result. Returns "" if NULL or out of bounds. */
const char *lcn_db_row_get(LcnDbResult *result, int row, const char *col_name);

/* Get numeric value from result. Returns 0 if NULL or out of bounds. */
int64_t lcn_db_row_get_number(LcnDbResult *result, int row, const char *col_name);

/* Get row count from result. */
int lcn_db_row_count(LcnDbResult *result);

/* Free a result set. */
void lcn_db_result_free(LcnDbResult *result);

/* Close connection and free resources. */
void lcn_db_close(LcnDbConn *conn);

/* Get last error message. */
const char *lcn_db_error(LcnDbConn *conn);

#endif /* LCN_MYSQL_DRIVER_H */
