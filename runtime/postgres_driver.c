/*
 * Limceron Runtime — PostgreSQL Driver
 * Binding over libpq (PostgreSQL C API).
 * C99, requires: -lpq
 *
 * Dual-path:
 *   - Compiled with -DLCN_HAS_POSTGRES: real database via libpq
 *   - Without: stub returning mock results (always compiles)
 */

#include "postgres_driver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LCN_HAS_POSTGRES

/* ════════════════════════════════════════════════════════════════
 * Real implementation (libpq)
 * ════════════════════════════════════════════════════════════════ */

#include <libpq-fe.h>

struct LcnPgConn {
    PGconn *pg;
    char    last_error[512];
};

LcnPgConn *lcn_pg_connect(const char *host, int port,
                           const char *user, const char *password,
                           const char *database)
{
    LcnPgConn *conn = (LcnPgConn *)calloc(1, sizeof(LcnPgConn));
    if (!conn) return NULL;

    /* Build connection string */
    char conninfo[1024];
    snprintf(conninfo, sizeof(conninfo),
             "host=%s port=%d user=%s password=%s dbname=%s "
             "connect_timeout=10 client_encoding=UTF8",
             host ? host : "localhost",
             port > 0 ? port : 5432,
             user ? user : "",
             password ? password : "",
             database ? database : "");

    conn->pg = PQconnectdb(conninfo);

    if (PQstatus(conn->pg) != CONNECTION_OK) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "connection failed: %s", PQerrorMessage(conn->pg));
        fprintf(stderr, "[pg] %s\n", conn->last_error);
        PQfinish(conn->pg);
        conn->pg = NULL;
        return conn;
    }

    fprintf(stderr, "[pg] connected to %s:%d/%s as %s\n",
            host ? host : "localhost",
            port > 0 ? port : 5432,
            database ? database : "",
            user ? user : "");
    return conn;
}

const char *lcn_pg_error(LcnPgConn *conn) {
    if (!conn) return "null connection";
    if (conn->pg) return PQerrorMessage(conn->pg);
    return conn->last_error;
}

void lcn_pg_close(LcnPgConn *conn) {
    if (!conn) return;
    if (conn->pg) {
        PQfinish(conn->pg);
        conn->pg = NULL;
    }
    free(conn);
    fprintf(stderr, "[pg] connection closed\n");
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

LcnPgResult *lcn_pg_query(LcnPgConn *conn, const char *sql)
{
    PGresult *res;
    LcnPgResult *result;
    int i, r;
    int num_fields, num_rows;

    if (!conn || !conn->pg || !sql) return NULL;

    res = PQexec(conn->pg, sql);

    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "query failed: %s", PQresultErrorMessage(res));
        fprintf(stderr, "[pg] %s\n", conn->last_error);
        PQclear(res);
        return NULL;
    }

    num_fields = PQnfields(res);
    num_rows = PQntuples(res);

    result = (LcnPgResult *)calloc(1, sizeof(LcnPgResult));
    if (!result) { PQclear(res); return NULL; }

    result->col_count = num_fields;

    /* Copy column names */
    result->col_names = (char **)malloc(sizeof(char *) * (size_t)num_fields);
    for (i = 0; i < num_fields; i++) {
        result->col_names[i] = safe_strdup(PQfname(res, i));
    }

    /* Copy all rows */
    result->_capacity = num_rows > 0 ? num_rows : 1;
    result->rows = (char ***)malloc(sizeof(char **) * (size_t)result->_capacity);
    result->row_count = num_rows;

    for (r = 0; r < num_rows; r++) {
        result->rows[r] = (char **)malloc(sizeof(char *) * (size_t)num_fields);
        for (i = 0; i < num_fields; i++) {
            if (PQgetisnull(res, r, i)) {
                result->rows[r][i] = safe_strdup("");
            } else {
                result->rows[r][i] = safe_strdup(PQgetvalue(res, r, i));
            }
        }
    }

    PQclear(res);
    return result;
}

/* ════════════════════════════════════════════════
 * Execute (INSERT/UPDATE/DELETE)
 * ════════════════════════════════════════════════ */

int64_t lcn_pg_execute(LcnPgConn *conn, const char *sql)
{
    PGresult *res;
    int64_t affected;

    if (!conn || !conn->pg || !sql) return -1;

    res = PQexec(conn->pg, sql);

    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        snprintf(conn->last_error, sizeof(conn->last_error),
                 "execute failed: %s", PQresultErrorMessage(res));
        fprintf(stderr, "[pg] %s\n", conn->last_error);
        PQclear(res);
        return -1;
    }

    {
        const char *ct = PQcmdTuples(res);
        affected = (ct && ct[0]) ? strtoll(ct, NULL, 10) : 0;
    }

    PQclear(res);
    return affected;
}

/* ════════════════════════════════════════════════
 * Escape
 * ════════════════════════════════════════════════ */

char *lcn_pg_escape(LcnPgConn *conn, const char *str)
{
    char *escaped;
    if (!conn || !conn->pg || !str) return safe_strdup("");

    escaped = PQescapeLiteral(conn->pg, str, strlen(str));
    if (!escaped) {
        /* PQescapeLiteral returns NULL on error */
        return safe_strdup("");
    }

    /* PQescapeLiteral returns PQ-allocated string; copy to malloc'd for caller */
    {
        char *copy = safe_strdup(escaped);
        PQfreemem(escaped);
        return copy;
    }
}

#else /* !LCN_HAS_POSTGRES */

/* ════════════════════════════════════════════════════════════════
 * Stub implementation (no libpq)
 * ════════════════════════════════════════════════════════════════ */

struct LcnPgConn {
    char last_error[512];
};

static char *safe_strdup(const char *s) {
    if (!s) return strdup("");
    return strdup(s);
}

LcnPgConn *lcn_pg_connect(const char *host, int port,
                           const char *user, const char *password,
                           const char *database)
{
    LcnPgConn *conn = (LcnPgConn *)calloc(1, sizeof(LcnPgConn));
    if (!conn) return NULL;

    snprintf(conn->last_error, sizeof(conn->last_error),
             "postgres driver not available (compile with -DLCN_HAS_POSTGRES -lpq)");
    fprintf(stderr, "[pg] stub: connect(%s:%d/%s) — libpq not linked\n",
            host ? host : "localhost",
            port > 0 ? port : 5432,
            database ? database : "");
    (void)user; (void)password;
    return conn;
}

const char *lcn_pg_error(LcnPgConn *conn) {
    if (!conn) return "null connection";
    return conn->last_error;
}

void lcn_pg_close(LcnPgConn *conn) {
    if (!conn) return;
    free(conn);
    fprintf(stderr, "[pg] stub: connection closed\n");
}

LcnPgResult *lcn_pg_query(LcnPgConn *conn, const char *sql) {
    LcnPgResult *result;
    if (!conn || !sql) return NULL;
    fprintf(stderr, "[pg] stub: query(%s)\n", sql);
    result = (LcnPgResult *)calloc(1, sizeof(LcnPgResult));
    return result;
}

int64_t lcn_pg_execute(LcnPgConn *conn, const char *sql) {
    if (!conn || !sql) return -1;
    fprintf(stderr, "[pg] stub: execute(%s)\n", sql);
    return 0;
}

char *lcn_pg_escape(LcnPgConn *conn, const char *str) {
    size_t len, i;
    char *out;
    size_t j;

    (void)conn;
    if (!str) return safe_strdup("''");

    /* Simple escape: double single quotes, wrap in quotes */
    len = strlen(str);
    out = (char *)malloc(len * 2 + 3);
    if (!out) return safe_strdup("''");

    j = 0;
    out[j++] = '\'';
    for (i = 0; i < len; i++) {
        if (str[i] == '\'') {
            out[j++] = '\'';
            out[j++] = '\'';
        } else if (str[i] == '\\') {
            out[j++] = '\\';
            out[j++] = '\\';
        } else {
            out[j++] = str[i];
        }
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}

#endif /* LCN_HAS_POSTGRES */

/* ════════════════════════════════════════════════
 * Result accessors (shared between real and stub)
 * ════════════════════════════════════════════════ */

int lcn_pg_row_count(LcnPgResult *result) {
    return result ? result->row_count : 0;
}

/* Find column index by name. Returns -1 if not found. */
static int pg_find_col(LcnPgResult *result, const char *name) {
    int i;
    if (!result || !name || !result->col_names) return -1;
    for (i = 0; i < result->col_count; i++) {
        if (result->col_names[i] && strcmp(result->col_names[i], name) == 0)
            return i;
    }
    return -1;
}

const char *lcn_pg_row_get(LcnPgResult *result, int row, const char *col_name) {
    int col;
    if (!result || row < 0 || row >= result->row_count) return "";
    col = pg_find_col(result, col_name);
    if (col < 0) return "";
    return result->rows[row][col] ? result->rows[row][col] : "";
}

int64_t lcn_pg_row_get_number(LcnPgResult *result, int row, const char *col_name) {
    const char *val = lcn_pg_row_get(result, row, col_name);
    if (!val || !val[0]) return 0;
    return strtoll(val, NULL, 10);
}

/* ════════════════════════════════════════════════
 * Cleanup
 * ════════════════════════════════════════════════ */

void lcn_pg_result_free(LcnPgResult *result) {
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
