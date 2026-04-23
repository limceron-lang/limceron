/*
 * Limceron HTTP/1.1 Server (Dashboard Backend)
 * Minimal select()-based server for the Limceron agent dashboard.
 * Pure C99, POSIX sockets, single-threaded.
 */
#ifndef LCN_HTTPD_H
#define LCN_HTTPD_H

#include <stdbool.h>
#include <stddef.h>

/* Maximum concurrent clients */
#define LCN_HTTPD_MAX_CLIENTS  16

/* HTTP request parsed from client */
typedef struct {
    char method[8];          /* GET, POST, etc. */
    char path[512];          /* Request path (decoded) */
    char query[512];         /* Query string (after ?) */
    char body[4096];         /* Request body */
    size_t body_len;
    int client_fd;           /* For sending response */
} LcnHttpRequest;

/* Route handler function type */
typedef void (*LcnRouteHandler)(const LcnHttpRequest *req);

/* Route entry */
typedef struct {
    char method[8];
    char pattern[128];       /* Path pattern (supports :param) */
    LcnRouteHandler handler;
} LcnRoute;

/* Server configuration */
typedef struct {
    int port;
    const char *static_dir;  /* Directory for static files (dashboard/) */
    LcnRoute *routes;
    int route_count;
    int server_fd;
    bool running;
} LcnHttpServer;

/* Initialize server */
void lcn_httpd_init(LcnHttpServer *server, int port, const char *static_dir);

/* Add a route */
void lcn_httpd_route(LcnHttpServer *server, const char *method,
                       const char *pattern, LcnRouteHandler handler);

/* Start listening (non-blocking setup) */
bool lcn_httpd_start(LcnHttpServer *server);

/* Poll for connections and handle requests (call in a loop or once) */
void lcn_httpd_poll(LcnHttpServer *server, int timeout_ms);

/* Stop server */
void lcn_httpd_stop(LcnHttpServer *server);

/* Response helpers — send to the request's client_fd */
void lcn_httpd_respond(const LcnHttpRequest *req, int status,
                         const char *content_type, const char *body,
                         size_t body_len);
void lcn_httpd_respond_json(const LcnHttpRequest *req, int status,
                              const char *json);
void lcn_httpd_respond_error(const LcnHttpRequest *req, int status,
                               const char *message);

/* Extract path parameter (e.g., :name from /api/agents/:name) */
bool lcn_httpd_path_param(const char *pattern, const char *actual_path,
                            const char *param_name, char *out, size_t out_size);

/* Extract query parameter */
const char *lcn_httpd_query_param(const LcnHttpRequest *req,
                                    const char *name, char *out,
                                    size_t out_size);

#endif /* LCN_HTTPD_H */
