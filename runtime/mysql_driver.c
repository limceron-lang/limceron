/*
 * Limceron Runtime — MySQL Driver
 * Binding over libmysqlclient (MySQL C API).
 * C99, requires: -lmysqlclient -I/opt/homebrew/opt/mysql-client/include
 */

#include "mysql_driver.h"
#include <mysql/mysql.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ════════════════════════════════════════════════
 * Connection
 * ════════════════════════════════════════════════ */

struct LcnDbConn {
    MYSQL *mysql;
    char   last_error[512];
};

LcnDbConn *lcn_db_connect(const char *host, const char *user,
                           const char *password, const char *database,
                           int port)
{
    LcnDbConn *conn = (LcnDbConn *)calloc(1, sizeof(LcnDbConn));
    if (!conn) return NULL;

    conn->mysql = mysql_init(NULL);
    if (!conn->mysql) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "mysql_init() failed");
        return conn;
    }

    /* Set UTF-8 before connecting */
    mysql_options(conn->mysql, MYSQL_SET_CHARSET_NAME, "utf8mb4");

    /* Set connect timeout */
    unsigned int timeout = 10;
    mysql_options(conn->mysql, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);

    /* Set read timeout */
    unsigned int read_timeout = 30;
    mysql_options(conn->mysql, MYSQL_OPT_READ_TIMEOUT, &read_timeout);

    if (!mysql_real_connect(conn->mysql, host, user, password, database,
                            port > 0 ? (unsigned int)port : 3306,
                            NULL, 0)) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "connection failed: %s", mysql_error(conn->mysql));
        fprintf(stderr, "[db] %s\n", conn->last_error);
        mysql_close(conn->mysql);
        conn->mysql = NULL;
        return conn;
    }

    fprintf(stderr, "[db] connected to %s:%d/%s as %s\n",
            host ? host : "localhost",
            port > 0 ? port : 3306,
            database ? database : "",
            user ? user : "");
    return conn;
}

const char *lcn_db_error(LcnDbConn *conn) {
    if (!conn) return "null connection";
    if (conn->mysql) return mysql_error(conn->mysql);
    return conn->last_error;
}

void lcn_db_close(LcnDbConn *conn) {
    if (!conn) return;
    if (conn->mysql) {
        mysql_close(conn->mysql);
        conn->mysql = NULL;
    }
    free(conn);
    fprintf(stderr, "[db] connection closed\n");
}

/* ════════════════════════════════════════════════
 * Helper: strdup with NULL safety
 * ════════════════════════════════════════════════ */

static char *safe_strdup(const char *s) {
    if (!s) return strdup("");
    return strdup(s);
}

/* ════════════════════════════════════════════════
 * Query (SELECT)
 * ════════════════════════════════════════════════ */

LcnDbResult *lcn_db_query(LcnDbConn *conn, const char *sql)
{
    MYSQL_RES *res;
    MYSQL_ROW row;
    MYSQL_FIELD *fields;
    LcnDbResult *result;
    int i;
    unsigned int num_fields;

    if (!conn || !conn->mysql || !sql) return NULL;

    if (mysql_real_query(conn->mysql, sql, (unsigned long)strlen(sql)) != 0) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "query failed: %s", mysql_error(conn->mysql));
        fprintf(stderr, "[db] %s\n", conn->last_error);
        return NULL;
    }

    res = mysql_store_result(conn->mysql);
    if (!res) {
        /* Might be a non-SELECT query or error */
        if (mysql_field_count(conn->mysql) == 0) {
            /* Not a SELECT — return empty result */
            result = (LcnDbResult *)calloc(1, sizeof(LcnDbResult));
            return result;
        }
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "store_result failed: %s", mysql_error(conn->mysql));
        return NULL;
    }

    num_fields = mysql_num_fields(res);
    fields = mysql_fetch_fields(res);

    result = (LcnDbResult *)calloc(1, sizeof(LcnDbResult));
    if (!result) { mysql_free_result(res); return NULL; }

    result->col_count = (int)num_fields;

    /* Copy column names */
    result->col_names = (char **)malloc(sizeof(char *) * num_fields);
    for (i = 0; i < (int)num_fields; i++) {
        result->col_names[i] = safe_strdup(fields[i].name);
    }

    /* Fetch all rows */
    result->_capacity = 64;
    result->rows = (char ***)malloc(sizeof(char **) * (size_t)result->_capacity);
    result->row_count = 0;

    while ((row = mysql_fetch_row(res)) != NULL) {
        unsigned long *lengths = mysql_fetch_lengths(res);

        if (result->row_count >= result->_capacity) {
            result->_capacity *= 2;
            result->rows = (char ***)realloc(result->rows,
                                              sizeof(char **) * (size_t)result->_capacity);
        }

        result->rows[result->row_count] = (char **)malloc(sizeof(char *) * num_fields);
        for (i = 0; i < (int)num_fields; i++) {
            if (row[i]) {
                result->rows[result->row_count][i] = (char *)malloc(lengths[i] + 1);
                memcpy(result->rows[result->row_count][i], row[i], lengths[i]);
                result->rows[result->row_count][i][lengths[i]] = '\0';
            } else {
                result->rows[result->row_count][i] = safe_strdup("");
            }
        }
        result->row_count++;
    }

    mysql_free_result(res);
    return result;
}

/* ════════════════════════════════════════════════
 * Execute (INSERT/UPDATE/DELETE)
 * ════════════════════════════════════════════════ */

int64_t lcn_db_execute(LcnDbConn *conn, const char *sql)
{
    if (!conn || !conn->mysql || !sql) return -1;

    if (mysql_real_query(conn->mysql, sql, (unsigned long)strlen(sql)) != 0) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "execute failed: %s", mysql_error(conn->mysql));
        fprintf(stderr, "[db] %s\n", conn->last_error);
        return -1;
    }

    int64_t affected = (int64_t)mysql_affected_rows(conn->mysql);
    return affected;
}

/* ════════════════════════════════════════════════
 * Result accessors
 * ════════════════════════════════════════════════ */

int lcn_db_row_count(LcnDbResult *result) {
    return result ? result->row_count : 0;
}

/* Find column index by name. Returns -1 if not found. */
static int find_col(LcnDbResult *result, const char *name) {
    int i;
    if (!result || !name || !result->col_names) return -1;
    for (i = 0; i < result->col_count; i++) {
        if (result->col_names[i] && strcmp(result->col_names[i], name) == 0)
            return i;
    }
    return -1;
}

const char *lcn_db_row_get(LcnDbResult *result, int row, const char *col_name) {
    int col;
    if (!result || row < 0 || row >= result->row_count) return "";
    col = find_col(result, col_name);
    if (col < 0) return "";
    return result->rows[row][col] ? result->rows[row][col] : "";
}

int64_t lcn_db_row_get_number(LcnDbResult *result, int row, const char *col_name) {
    const char *val = lcn_db_row_get(result, row, col_name);
    if (!val || !val[0]) return 0;
    return strtoll(val, NULL, 10);
}

/* ════════════════════════════════════════════════
 * Cleanup
 * ════════════════════════════════════════════════ */

void lcn_db_result_free(LcnDbResult *result) {
    int r, c;
    if (!result) return;
    for (r = 0; r < result->row_count; r++) {
        if (result->rows[r]) {
            for (c = 0; c < result->col_count; c++) {
                free(result->rows[r][c]);
            }
            free(result->rows[r]);
        }
    }
    free(result->rows);
    if (result->col_names) {
        for (c = 0; c < result->col_count; c++) {
            free(result->col_names[c]);
        }
        free(result->col_names);
    }
    free(result);
}
