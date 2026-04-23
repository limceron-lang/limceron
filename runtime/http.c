/*
 * Limceron HTTP Client
 * Pure C99, POSIX sockets. No TLS.
 * Designed for localhost (Ollama at localhost:11434).
 * Supports chunked transfer encoding.
 */
#include "http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>

/* -------------------------------------------------------------------------- */
/*  Internal constants                                                        */
/* -------------------------------------------------------------------------- */

#define HTTP_RECV_BUF   4096
#define HTTP_TIMEOUT_S  30
#define HTTP_INIT_BUF   8192

/* -------------------------------------------------------------------------- */
/*  Growing buffer                                                            */
/* -------------------------------------------------------------------------- */

typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} GrowBuf;

static GrowBuf growbuf_new(size_t initial)
{
    GrowBuf gb;
    gb.cap = initial > 0 ? initial : HTTP_INIT_BUF;
    gb.len = 0;
    gb.data = (char *)malloc(gb.cap);
    return gb;
}

static int growbuf_append(GrowBuf *gb, const char *data, size_t len)
{
    if (!gb->data) return -1;
    while (gb->len + len > gb->cap) {
        size_t newcap = gb->cap * 2;
        if (newcap < gb->len + len) newcap = gb->len + len;
        char *nb = (char *)realloc(gb->data, newcap);
        if (!nb) { free(gb->data); gb->data = NULL; return -1; }
        gb->data = nb;
        gb->cap = newcap;
    }
    memcpy(gb->data + gb->len, data, len);
    gb->len += len;
    return 0;
}

static void growbuf_free(GrowBuf *gb)
{
    free(gb->data);
    gb->data = NULL;
    gb->len = 0;
    gb->cap = 0;
}

/* -------------------------------------------------------------------------- */
/*  URL parsing                                                               */
/* -------------------------------------------------------------------------- */

typedef struct {
    char host[256];
    int  port;
    char path[2048];
} ParsedUrl;

static int parse_url(const char *url, ParsedUrl *out)
{
    const char *p = url;
    const char *host_start;
    const char *host_end;
    size_t host_len;

    memset(out, 0, sizeof(ParsedUrl));
    out->port = 80;

    /* Skip scheme */
    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        /* We don't support TLS, but parse it anyway */
        p += 8;
        out->port = 443;
    }

    host_start = p;

    /* Find end of host (: or / or end) */
    host_end = p;
    while (*host_end && *host_end != ':' && *host_end != '/') {
        host_end++;
    }

    host_len = (size_t)(host_end - host_start);
    if (host_len == 0 || host_len >= sizeof(out->host)) return -1;
    memcpy(out->host, host_start, host_len);
    out->host[host_len] = '\0';

    p = host_end;

    /* Parse port if present */
    if (*p == ':') {
        p++;
        {
            char *end = NULL;
            long port = strtol(p, &end, 10);
            if (end == p || port <= 0 || port > 65535) return -1;
            out->port = (int)port;
            p = end;
        }
    }

    /* Parse path */
    if (*p == '/') {
        size_t path_len = strlen(p);
        if (path_len >= sizeof(out->path)) return -1;
        memcpy(out->path, p, path_len + 1);
    } else {
        out->path[0] = '/';
        out->path[1] = '\0';
    }

    return 0;
}

/* -------------------------------------------------------------------------- */
/*  TCP connection                                                            */
/* -------------------------------------------------------------------------- */

static int tcp_connect(const char *host, int port)
{
    struct addrinfo hints, *res, *rp;
    char port_str[16];
    int fd = -1;
    int ret;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    snprintf(port_str, sizeof(port_str), "%d", port);

    ret = getaddrinfo(host, port_str, &hints, &res);
    if (ret != 0) return -1;

    for (rp = res; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            break; /* success */
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* -------------------------------------------------------------------------- */
/*  Send all data                                                             */
/* -------------------------------------------------------------------------- */

static int send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = write(fd, data + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Read all data from socket                                                 */
/* -------------------------------------------------------------------------- */

static int recv_all(int fd, GrowBuf *gb)
{
    char buf[HTTP_RECV_BUF];
    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) break; /* connection closed */
        if (growbuf_append(gb, buf, (size_t)n) != 0) return -1;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Case-insensitive prefix match                                             */
/* -------------------------------------------------------------------------- */

static int strncasecmp_local(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        int ca = (unsigned char)tolower((unsigned char)a[i]);
        int cb = (unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

/* -------------------------------------------------------------------------- */
/*  Find header value in raw headers (case-insensitive)                       */
/* -------------------------------------------------------------------------- */

static const char *find_header(const char *headers, size_t headers_len,
                               const char *name, size_t *value_len)
{
    size_t name_len = strlen(name);
    const char *p = headers;
    const char *end = headers + headers_len;

    while (p < end) {
        /* Find end of this line */
        const char *line_end = p;
        while (line_end < end && *line_end != '\r' && *line_end != '\n') {
            line_end++;
        }

        /* Check if this line starts with the header name */
        if ((size_t)(line_end - p) > name_len + 1 &&
            strncasecmp_local(p, name, name_len) == 0 &&
            p[name_len] == ':') {
            const char *val = p + name_len + 1;
            /* Skip leading whitespace */
            while (val < line_end && (*val == ' ' || *val == '\t')) val++;
            if (value_len) *value_len = (size_t)(line_end - val);
            return val;
        }

        /* Skip line ending */
        if (line_end < end && *line_end == '\r') line_end++;
        if (line_end < end && *line_end == '\n') line_end++;
        p = line_end;
    }
    return NULL;
}

/* -------------------------------------------------------------------------- */
/*  Parse chunked body                                                        */
/* -------------------------------------------------------------------------- */

static char *decode_chunked(const char *data, size_t data_len, size_t *out_len)
{
    GrowBuf gb = growbuf_new(data_len);
    if (!gb.data) return NULL;

    const char *p = data;
    const char *end = data + data_len;

    while (p < end) {
        /* Parse chunk size (hex) */
        char *size_end = NULL;
        unsigned long chunk_size;

        /* Skip any leading \r\n (between chunks) */
        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') p += 2;

        errno = 0;
        chunk_size = strtoul(p, &size_end, 16);
        if (size_end == p || errno != 0) {
            growbuf_free(&gb);
            return NULL;
        }
        p = size_end;

        /* Skip optional chunk extensions and CRLF */
        while (p < end && *p != '\r' && *p != '\n') p++;
        if (p + 1 < end && p[0] == '\r' && p[1] == '\n') p += 2;
        else if (p < end && p[0] == '\n') p++;

        if (chunk_size == 0) break; /* last chunk */

        /* Read chunk data */
        if ((size_t)(end - p) < chunk_size) {
            growbuf_free(&gb);
            return NULL;
        }

        if (growbuf_append(&gb, p, chunk_size) != 0) {
            growbuf_free(&gb);
            return NULL;
        }
        p += chunk_size;
    }

    /* NUL-terminate for convenience */
    if (growbuf_append(&gb, "\0", 1) != 0) {
        growbuf_free(&gb);
        return NULL;
    }
    gb.len--; /* don't count NUL in length */

    if (out_len) *out_len = gb.len;
    return gb.data;
}

/* -------------------------------------------------------------------------- */
/*  Parse HTTP response                                                       */
/* -------------------------------------------------------------------------- */

static LcnHttpResponse *parse_response(GrowBuf *raw)
{
    LcnHttpResponse *resp;
    const char *header_end;
    const char *body_start;
    size_t body_len;
    int status;
    const char *p;

    if (!raw->data || raw->len == 0) return NULL;

    /* Find end of headers */
    header_end = NULL;
    {
        size_t i;
        for (i = 0; i + 3 < raw->len; i++) {
            if (raw->data[i] == '\r' && raw->data[i+1] == '\n' &&
                raw->data[i+2] == '\r' && raw->data[i+3] == '\n') {
                header_end = raw->data + i;
                break;
            }
        }
    }

    if (!header_end) return NULL;

    body_start = header_end + 4;
    body_len = raw->len - (size_t)(body_start - raw->data);

    /* Parse status line: HTTP/1.x NNN ... */
    p = raw->data;
    if (raw->len < 12) return NULL;
    if (strncmp(p, "HTTP/1.", 7) != 0) return NULL;
    p += 9; /* skip "HTTP/1.X " */
    status = (p[0] - '0') * 100 + (p[1] - '0') * 10 + (p[2] - '0');

    /* Find start of actual headers (after status line) */
    {
        const char *status_line_end = raw->data;
        while (status_line_end < header_end &&
               *status_line_end != '\r' && *status_line_end != '\n') {
            status_line_end++;
        }
        if (status_line_end + 2 <= header_end &&
            status_line_end[0] == '\r' && status_line_end[1] == '\n') {
            status_line_end += 2;
        }

        resp = (LcnHttpResponse *)calloc(1, sizeof(LcnHttpResponse));
        if (!resp) return NULL;
        resp->status_code = status;

        /* Copy headers (without status line) */
        {
            size_t hdr_len = (size_t)(header_end - status_line_end);
            resp->headers = (char *)malloc(hdr_len + 1);
            if (!resp->headers) { free(resp); return NULL; }
            memcpy(resp->headers, status_line_end, hdr_len);
            resp->headers[hdr_len] = '\0';
            resp->headers_len = hdr_len;
        }
    }

    /* Check for chunked transfer encoding */
    {
        size_t te_len = 0;
        const char *te = find_header(resp->headers, resp->headers_len,
                                     "Transfer-Encoding", &te_len);
        if (te && te_len >= 7 && strncasecmp_local(te, "chunked", 7) == 0) {
            resp->body = decode_chunked(body_start, body_len, &resp->body_len);
            if (!resp->body) {
                free(resp->headers);
                free(resp);
                return NULL;
            }
            return resp;
        }
    }

    /* Check for Content-Length */
    {
        size_t cl_len = 0;
        const char *cl = find_header(resp->headers, resp->headers_len,
                                     "Content-Length", &cl_len);
        if (cl) {
            char cl_buf[32];
            unsigned long content_length;
            size_t copy_len = cl_len < sizeof(cl_buf) - 1 ? cl_len : sizeof(cl_buf) - 1;
            memcpy(cl_buf, cl, copy_len);
            cl_buf[copy_len] = '\0';
            content_length = strtoul(cl_buf, NULL, 10);
            if (content_length < body_len) body_len = content_length;
        }
    }

    /* Copy body */
    resp->body = (char *)malloc(body_len + 1);
    if (!resp->body) {
        free(resp->headers);
        free(resp);
        return NULL;
    }
    memcpy(resp->body, body_start, body_len);
    resp->body[body_len] = '\0';
    resp->body_len = body_len;

    return resp;
}

/* -------------------------------------------------------------------------- */
/*  Internal: perform HTTP request                                            */
/* -------------------------------------------------------------------------- */

static LcnHttpResponse *do_request_ex(const char *method, const char *url,
                                       const char *body, const char *content_type,
                                       const char *extra_headers)
{
    ParsedUrl parsed;
    int fd;
    GrowBuf request;
    GrowBuf response;
    LcnHttpResponse *resp;
    struct timeval tv;
    char line_buf[256];
    size_t body_len;

    if (parse_url(url, &parsed) != 0) return NULL;

    fd = tcp_connect(parsed.host, parsed.port);
    if (fd < 0) return NULL;

    /* Set receive timeout */
    tv.tv_sec = HTTP_TIMEOUT_S;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build request */
    request = growbuf_new(1024);
    if (!request.data) { close(fd); return NULL; }

    snprintf(line_buf, sizeof(line_buf), "%s %s HTTP/1.1\r\n", method, parsed.path);
    growbuf_append(&request, line_buf, strlen(line_buf));

    /* Host header */
    if (parsed.port == 80) {
        snprintf(line_buf, sizeof(line_buf), "Host: %s\r\n", parsed.host);
    } else {
        snprintf(line_buf, sizeof(line_buf), "Host: %s:%d\r\n",
                 parsed.host, parsed.port);
    }
    growbuf_append(&request, line_buf, strlen(line_buf));

    /* Connection: close */
    growbuf_append(&request, "Connection: close\r\n", 19);

    /* User-Agent */
    growbuf_append(&request, "User-Agent: Limceron/0.1\r\n", 26);

    /* Accept */
    growbuf_append(&request, "Accept: */*\r\n", 13);

    body_len = body ? strlen(body) : 0;

    if (body && body_len > 0) {
        if (content_type) {
            snprintf(line_buf, sizeof(line_buf), "Content-Type: %s\r\n",
                     content_type);
            growbuf_append(&request, line_buf, strlen(line_buf));
        }
        snprintf(line_buf, sizeof(line_buf), "Content-Length: %zu\r\n", body_len);
        growbuf_append(&request, line_buf, strlen(line_buf));
    }

    /* Extra headers (e.g., Authorization) */
    if (extra_headers && extra_headers[0]) {
        growbuf_append(&request, extra_headers, strlen(extra_headers));
    }

    /* End of headers */
    growbuf_append(&request, "\r\n", 2);

    /* Body */
    if (body && body_len > 0) {
        growbuf_append(&request, body, body_len);
    }

    /* Send request */
    if (send_all(fd, request.data, request.len) != 0) {
        growbuf_free(&request);
        close(fd);
        return NULL;
    }
    growbuf_free(&request);

    /* Read response */
    response = growbuf_new(HTTP_INIT_BUF);
    if (!response.data) { close(fd); return NULL; }

    if (recv_all(fd, &response) != 0 && response.len == 0) {
        growbuf_free(&response);
        close(fd);
        return NULL;
    }
    close(fd);

    /* Parse response */
    resp = parse_response(&response);
    growbuf_free(&response);
    return resp;
}

/* -------------------------------------------------------------------------- */
/*  Public API                                                                */
/* -------------------------------------------------------------------------- */

LcnHttpResponse *lcn_http_post(const char *url, const char *body,
                                 const char *content_type)
{
    if (!url) return NULL;
    return do_request_ex("POST", url, body, content_type, NULL);
}

LcnHttpResponse *lcn_http_post_with_headers(const char *url, const char *body,
                                             const char *content_type,
                                             const char *extra_headers)
{
    if (!url) return NULL;
    return do_request_ex("POST", url, body, content_type, extra_headers);
}

LcnHttpResponse *lcn_http_get(const char *url)
{
    if (!url) return NULL;
    return do_request_ex("GET", url, NULL, NULL, NULL);
}

void lcn_http_response_free(LcnHttpResponse *resp)
{
    if (!resp) return;
    free(resp->body);
    free(resp->headers);
    free(resp);
}

char *lcn_http_get_header(const LcnHttpResponse *resp, const char *name)
{
    size_t val_len = 0;
    const char *val;
    char *result;

    if (!resp || !resp->headers || !name) return NULL;

    val = find_header(resp->headers, resp->headers_len, name, &val_len);
    if (!val) return NULL;

    result = (char *)malloc(val_len + 1);
    if (!result) return NULL;
    memcpy(result, val, val_len);
    result[val_len] = '\0';
    return result;
}
