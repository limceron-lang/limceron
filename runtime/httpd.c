/*
 * Limceron HTTP/1.1 Server — Implementation
 * Minimal select()-based server for the Limceron agent dashboard.
 * Pure C99, POSIX sockets, single-threaded (Stage 0).
 */

#include "httpd.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------- */
/*  Internal constants                                                        */
/* -------------------------------------------------------------------------- */

#define HTTPD_MAX_REQUEST   8192
#define HTTPD_MAX_ROUTES    32
#define HTTPD_INIT_ROUTES   8
#define HTTPD_BACKLOG       16
#define HTTPD_DEFAULT_PORT  9090
#define HTTPD_MAX_PATH      1024

/* -------------------------------------------------------------------------- */
/*  MIME type detection                                                       */
/* -------------------------------------------------------------------------- */

typedef struct {
    const char *ext;
    const char *mime;
} MimeEntry;

static const MimeEntry mime_table[] = {
    { ".html", "text/html"               },
    { ".css",  "text/css"                },
    { ".js",   "application/javascript"  },
    { ".json", "application/json"        },
    { ".svg",  "image/svg+xml"           },
    { ".png",  "image/png"               },
    { ".ico",  "image/x-icon"            },
    { NULL,    NULL                       }
};

static const char *detect_mime(const char *path)
{
    const char *dot;
    const MimeEntry *m;

    dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";

    for (m = mime_table; m->ext; m++) {
        if (strcmp(dot, m->ext) == 0) {
            return m->mime;
        }
    }
    return "application/octet-stream";
}

/* -------------------------------------------------------------------------- */
/*  HTTP status reason phrases                                                */
/* -------------------------------------------------------------------------- */

static const char *status_reason(int status)
{
    switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
    }
}

/* -------------------------------------------------------------------------- */
/*  URL decoding                                                              */
/* -------------------------------------------------------------------------- */

static int hex_digit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static size_t url_decode(const char *src, size_t src_len, char *dst,
                          size_t dst_size)
{
    size_t si = 0;
    size_t di = 0;

    while (si < src_len && di < dst_size - 1) {
        if (src[si] == '%' && si + 2 < src_len) {
            int hi = hex_digit(src[si + 1]);
            int lo = hex_digit(src[si + 2]);
            if (hi >= 0 && lo >= 0) {
                dst[di++] = (char)((hi << 4) | lo);
                si += 3;
                continue;
            }
        }
        if (src[si] == '+') {
            dst[di++] = ' ';
            si++;
        } else {
            dst[di++] = src[si++];
        }
    }
    dst[di] = '\0';
    return di;
}

/* -------------------------------------------------------------------------- */
/*  Path security: reject directory traversal                                 */
/* -------------------------------------------------------------------------- */

static bool path_is_safe(const char *path)
{
    const char *p = path;
    while (*p) {
        if (p[0] == '.' && p[1] == '.') {
            if (p[2] == '/' || p[2] == '\0') return false;
        }
        p++;
    }
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Request parsing                                                           */
/* -------------------------------------------------------------------------- */

static bool parse_request(const char *raw, size_t raw_len, LcnHttpRequest *req)
{
    const char *p = raw;
    const char *end = raw + raw_len;
    const char *line_end;
    const char *space;
    const char *path_start;
    const char *path_end;
    const char *query_start;
    size_t method_len;
    size_t path_len;
    const char *body_start;

    memset(req, 0, sizeof(LcnHttpRequest));

    /* Find end of request line */
    line_end = p;
    while (line_end < end && *line_end != '\r' && *line_end != '\n') {
        line_end++;
    }

    /* Parse method */
    space = memchr(p, ' ', (size_t)(line_end - p));
    if (!space) return false;

    method_len = (size_t)(space - p);
    if (method_len >= sizeof(req->method)) return false;
    memcpy(req->method, p, method_len);
    req->method[method_len] = '\0';

    /* Parse URI */
    p = space + 1;
    space = memchr(p, ' ', (size_t)(line_end - p));
    if (!space) return false;

    path_start = p;
    path_end = space;

    /* Split path and query string */
    query_start = memchr(path_start, '?', (size_t)(path_end - path_start));
    if (query_start) {
        /* Decode path up to ? */
        url_decode(path_start, (size_t)(query_start - path_start),
                   req->path, sizeof(req->path));
        /* Decode query string after ? */
        url_decode(query_start + 1, (size_t)(path_end - query_start - 1),
                   req->query, sizeof(req->query));
    } else {
        url_decode(path_start, (size_t)(path_end - path_start),
                   req->path, sizeof(req->path));
        req->query[0] = '\0';
    }

    /* Normalize: ensure path starts with / */
    path_len = strlen(req->path);
    if (path_len == 0 || req->path[0] != '/') {
        return false;
    }

    /* Strip trailing slash (except root) */
    if (path_len > 1 && req->path[path_len - 1] == '/') {
        req->path[path_len - 1] = '\0';
    }

    /* Security: reject path traversal */
    if (!path_is_safe(req->path)) return false;

    /* Find body (after \r\n\r\n) */
    body_start = NULL;
    {
        size_t i;
        for (i = 0; i + 3 < raw_len; i++) {
            if (raw[i] == '\r' && raw[i+1] == '\n' &&
                raw[i+2] == '\r' && raw[i+3] == '\n') {
                body_start = raw + i + 4;
                break;
            }
        }
    }

    if (body_start && body_start < end) {
        size_t body_avail = (size_t)(end - body_start);
        if (body_avail > sizeof(req->body) - 1) {
            body_avail = sizeof(req->body) - 1;
        }
        memcpy(req->body, body_start, body_avail);
        req->body[body_avail] = '\0';
        req->body_len = body_avail;
    }

    return true;
}

/* -------------------------------------------------------------------------- */
/*  Route matching                                                            */
/* -------------------------------------------------------------------------- */

/*
 * Match a pattern against a path, segment by segment.
 * Pattern segments starting with ':' match any single segment.
 * Returns true on match.
 */
static bool route_match(const char *pattern, const char *path)
{
    const char *pp = pattern;
    const char *ap = path;

    /* Both must start with / */
    if (*pp != '/' || *ap != '/') return false;

    while (*pp && *ap) {
        const char *pat_seg_start;
        const char *pat_seg_end;
        const char *act_seg_start;
        const char *act_seg_end;

        /* Skip leading slash */
        if (*pp == '/') pp++;
        if (*ap == '/') ap++;

        /* Find end of pattern segment */
        pat_seg_start = pp;
        pat_seg_end = pp;
        while (*pat_seg_end && *pat_seg_end != '/') pat_seg_end++;

        /* Find end of actual segment */
        act_seg_start = ap;
        act_seg_end = ap;
        while (*act_seg_end && *act_seg_end != '/') act_seg_end++;

        /* Check: empty segments */
        if (pat_seg_start == pat_seg_end && act_seg_start == act_seg_end) {
            break;
        }
        if (pat_seg_start == pat_seg_end || act_seg_start == act_seg_end) {
            return false;
        }

        /* :param matches any segment */
        if (*pat_seg_start != ':') {
            size_t plen = (size_t)(pat_seg_end - pat_seg_start);
            size_t alen = (size_t)(act_seg_end - act_seg_start);
            if (plen != alen) return false;
            if (memcmp(pat_seg_start, act_seg_start, plen) != 0) return false;
        }

        pp = pat_seg_end;
        ap = act_seg_end;
    }

    /* Both must be exhausted */
    if (*pp == '/' && *(pp + 1) == '\0') pp++;
    if (*ap == '/' && *(ap + 1) == '\0') ap++;

    return (*pp == '\0' && *ap == '\0');
}

/* -------------------------------------------------------------------------- */
/*  Path parameter extraction                                                 */
/* -------------------------------------------------------------------------- */

bool lcn_httpd_path_param(const char *pattern, const char *actual_path,
                            const char *param_name, char *out, size_t out_size)
{
    const char *pp = pattern;
    const char *ap = actual_path;

    if (!pattern || !actual_path || !param_name || !out || out_size == 0) {
        return false;
    }

    while (*pp && *ap) {
        const char *pat_seg_start;
        const char *pat_seg_end;
        const char *act_seg_start;
        const char *act_seg_end;
        size_t seg_len;

        /* Skip leading slash */
        if (*pp == '/') pp++;
        if (*ap == '/') ap++;

        pat_seg_start = pp;
        pat_seg_end = pp;
        while (*pat_seg_end && *pat_seg_end != '/') pat_seg_end++;

        act_seg_start = ap;
        act_seg_end = ap;
        while (*act_seg_end && *act_seg_end != '/') act_seg_end++;

        /* Check if this pattern segment is our parameter */
        if (*pat_seg_start == ':') {
            size_t name_len = (size_t)(pat_seg_end - pat_seg_start - 1);
            if (name_len == strlen(param_name) &&
                memcmp(pat_seg_start + 1, param_name, name_len) == 0) {
                seg_len = (size_t)(act_seg_end - act_seg_start);
                if (seg_len >= out_size) seg_len = out_size - 1;
                memcpy(out, act_seg_start, seg_len);
                out[seg_len] = '\0';
                return true;
            }
        }

        pp = pat_seg_end;
        ap = act_seg_end;
    }

    return false;
}

/* -------------------------------------------------------------------------- */
/*  Query parameter extraction                                                */
/* -------------------------------------------------------------------------- */

const char *lcn_httpd_query_param(const LcnHttpRequest *req,
                                    const char *name, char *out,
                                    size_t out_size)
{
    const char *p;
    size_t name_len;

    if (!req || !name || !out || out_size == 0) return NULL;
    if (req->query[0] == '\0') return NULL;

    p = req->query;
    name_len = strlen(name);

    while (*p) {
        const char *key_start;
        const char *key_end;
        const char *val_start;
        const char *val_end;
        size_t key_len;
        size_t val_len;

        /* Find key */
        key_start = p;
        key_end = p;
        while (*key_end && *key_end != '=' && *key_end != '&') key_end++;

        key_len = (size_t)(key_end - key_start);

        /* Find value */
        val_start = NULL;
        val_end = key_end;
        if (*key_end == '=') {
            val_start = key_end + 1;
            val_end = val_start;
            while (*val_end && *val_end != '&') val_end++;
        }

        /* Check if this is our parameter */
        if (key_len == name_len && memcmp(key_start, name, key_len) == 0) {
            if (val_start) {
                val_len = (size_t)(val_end - val_start);
                url_decode(val_start, val_len, out, out_size);
            } else {
                out[0] = '\0';
            }
            return out;
        }

        /* Advance to next parameter */
        p = val_end;
        if (*p == '&') p++;
    }

    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Response helpers                                                          */
/* -------------------------------------------------------------------------- */

void lcn_httpd_respond(const LcnHttpRequest *req, int status,
                         const char *content_type, const char *body,
                         size_t body_len)
{
    char header[512];
    int header_len;
    int fd;

    if (!req) return;
    fd = req->client_fd;
    if (fd < 0) return;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "\r\n",
        status, status_reason(status),
        content_type ? content_type : "application/octet-stream",
        body_len);

    if (header_len > 0) {
        (void)write(fd, header, (size_t)header_len);
    }
    if (body && body_len > 0) {
        (void)write(fd, body, body_len);
    }
}

void lcn_httpd_respond_json(const LcnHttpRequest *req, int status,
                              const char *json)
{
    size_t len;

    if (!json) {
        lcn_httpd_respond(req, status, "application/json", "{}", 2);
        return;
    }
    len = strlen(json);
    lcn_httpd_respond(req, status, "application/json", json, len);
}

void lcn_httpd_respond_error(const LcnHttpRequest *req, int status,
                               const char *message)
{
    char buf[512];
    int len;

    len = snprintf(buf, sizeof(buf),
                   "{\"error\":\"%s\",\"status\":%d}",
                   message ? message : "unknown error",
                   status);
    if (len < 0) len = 0;
    lcn_httpd_respond(req, status, "application/json", buf, (size_t)len);
}

/* -------------------------------------------------------------------------- */
/*  Static file serving                                                       */
/* -------------------------------------------------------------------------- */

static void serve_static_file(const LcnHttpServer *server,
                               const LcnHttpRequest *req)
{
    char filepath[HTTPD_MAX_PATH];
    struct stat st;
    FILE *fp;
    char *buf;
    size_t nread;
    const char *serve_path;
    const char *mime;
    int n;

    if (!server->static_dir) {
        lcn_httpd_respond_error(req, 404, "not found");
        return;
    }

    /* Map / to /index.html */
    serve_path = req->path;
    if (strcmp(serve_path, "/") == 0) {
        serve_path = "/index.html";
    }

    n = snprintf(filepath, sizeof(filepath), "%s%s",
                 server->static_dir, serve_path);
    if (n < 0 || (size_t)n >= sizeof(filepath)) {
        lcn_httpd_respond_error(req, 400, "path too long");
        return;
    }

    /* Security: re-check for traversal in the resolved path */
    if (!path_is_safe(filepath)) {
        lcn_httpd_respond_error(req, 403, "forbidden");
        return;
    }

    if (stat(filepath, &st) != 0 || !S_ISREG(st.st_mode)) {
        lcn_httpd_respond_error(req, 404, "not found");
        return;
    }

    fp = fopen(filepath, "rb");
    if (!fp) {
        lcn_httpd_respond_error(req, 500, "cannot read file");
        return;
    }

    buf = (char *)malloc((size_t)st.st_size);
    if (!buf) {
        fclose(fp);
        lcn_httpd_respond_error(req, 500, "out of memory");
        return;
    }

    nread = fread(buf, 1, (size_t)st.st_size, fp);
    fclose(fp);

    mime = detect_mime(filepath);
    lcn_httpd_respond(req, 200, mime, buf, nread);
    free(buf);
}

/* -------------------------------------------------------------------------- */
/*  OPTIONS preflight handler                                                 */
/* -------------------------------------------------------------------------- */

static void handle_options(const LcnHttpRequest *req)
{
    char header[512];
    int header_len;
    int fd;

    if (!req) return;
    fd = req->client_fd;
    if (fd < 0) return;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, POST, PUT, DELETE, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type, Authorization\r\n"
        "Access-Control-Max-Age: 86400\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n");

    if (header_len > 0) {
        (void)write(fd, header, (size_t)header_len);
    }
}

/* -------------------------------------------------------------------------- */
/*  Request dispatch                                                          */
/* -------------------------------------------------------------------------- */

static void dispatch_request(LcnHttpServer *server, LcnHttpRequest *req)
{
    int i;

    /* Handle CORS preflight */
    if (strcmp(req->method, "OPTIONS") == 0) {
        handle_options(req);
        return;
    }

    /* Try routes */
    for (i = 0; i < server->route_count; i++) {
        LcnRoute *r = &server->routes[i];

        if (strcmp(r->method, req->method) != 0) continue;
        if (!route_match(r->pattern, req->path)) continue;

        /* Found a matching route */
        r->handler(req);
        return;
    }

    /* No route matched — try static files */
    if (server->static_dir && strcmp(req->method, "GET") == 0) {
        serve_static_file(server, req);
        return;
    }

    lcn_httpd_respond_error(req, 404, "not found");
}

/* -------------------------------------------------------------------------- */
/*  Public API: init                                                          */
/* -------------------------------------------------------------------------- */

void lcn_httpd_init(LcnHttpServer *server, int port, const char *static_dir)
{
    if (!server) return;

    memset(server, 0, sizeof(LcnHttpServer));
    server->port = (port > 0) ? port : HTTPD_DEFAULT_PORT;
    server->static_dir = static_dir;
    server->server_fd = -1;
    server->running = false;
    server->route_count = 0;

    server->routes = (LcnRoute *)calloc(HTTPD_INIT_ROUTES, sizeof(LcnRoute));
    /* route_count stays 0; capacity tracked implicitly (max HTTPD_MAX_ROUTES) */
}

/* -------------------------------------------------------------------------- */
/*  Public API: add route                                                     */
/* -------------------------------------------------------------------------- */

void lcn_httpd_route(LcnHttpServer *server, const char *method,
                       const char *pattern, LcnRouteHandler handler)
{
    LcnRoute *r;

    if (!server || !method || !pattern || !handler) return;
    if (server->route_count >= HTTPD_MAX_ROUTES) return;

    r = &server->routes[server->route_count];
    snprintf(r->method, sizeof(r->method), "%s", method);
    snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
    r->handler = handler;
    server->route_count++;
}

/* -------------------------------------------------------------------------- */
/*  Public API: start                                                         */
/* -------------------------------------------------------------------------- */

bool lcn_httpd_start(LcnHttpServer *server)
{
    struct sockaddr_in addr;
    int opt = 1;
    int fd;

    if (!server) return false;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "[limceron-httpd] socket() failed: %s\n", strerror(errno));
        return false;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        fprintf(stderr, "[limceron-httpd] setsockopt() failed: %s\n",
                strerror(errno));
        close(fd);
        return false;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)server->port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[limceron-httpd] bind(:%d) failed: %s\n",
                server->port, strerror(errno));
        close(fd);
        return false;
    }

    if (listen(fd, HTTPD_BACKLOG) < 0) {
        fprintf(stderr, "[limceron-httpd] listen() failed: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    server->server_fd = fd;
    server->running = true;

    fprintf(stderr, "[limceron-httpd] listening on http://0.0.0.0:%d\n",
            server->port);
    return true;
}

/* -------------------------------------------------------------------------- */
/*  Public API: poll                                                          */
/* -------------------------------------------------------------------------- */

void lcn_httpd_poll(LcnHttpServer *server, int timeout_ms)
{
    fd_set readfds;
    struct timeval tv;
    int nfds;
    int ret;
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_len;
    char buf[HTTPD_MAX_REQUEST];
    ssize_t nread;
    LcnHttpRequest req;

    if (!server || !server->running || server->server_fd < 0) return;

    FD_ZERO(&readfds);
    FD_SET(server->server_fd, &readfds);
    nfds = server->server_fd + 1;

    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    ret = select(nfds, &readfds, NULL, NULL, &tv);
    if (ret <= 0) return;

    if (!FD_ISSET(server->server_fd, &readfds)) return;

    client_len = sizeof(client_addr);
    client_fd = accept(server->server_fd,
                       (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) return;

    /* Read the full request (non-persistent: read once, respond, close) */
    nread = read(client_fd, buf, sizeof(buf) - 1);
    if (nread <= 0) {
        close(client_fd);
        return;
    }
    buf[nread] = '\0';

    /* Parse and dispatch */
    if (parse_request(buf, (size_t)nread, &req)) {
        req.client_fd = client_fd;
        dispatch_request(server, &req);
    } else {
        /* Malformed request */
        memset(&req, 0, sizeof(req));
        req.client_fd = client_fd;
        lcn_httpd_respond_error(&req, 400, "bad request");
    }

    close(client_fd);
}

/* -------------------------------------------------------------------------- */
/*  Public API: stop                                                          */
/* -------------------------------------------------------------------------- */

void lcn_httpd_stop(LcnHttpServer *server)
{
    if (!server) return;

    server->running = false;

    if (server->server_fd >= 0) {
        close(server->server_fd);
        server->server_fd = -1;
    }

    free(server->routes);
    server->routes = NULL;
    server->route_count = 0;

    fprintf(stderr, "[limceron-httpd] stopped\n");
}
