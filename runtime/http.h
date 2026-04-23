/*
 * Limceron HTTP Client
 * Pure C99, POSIX sockets only. No TLS.
 * Designed for localhost communication (e.g., Ollama at localhost:11434).
 * Supports chunked transfer encoding.
 */
#ifndef LCN_HTTP_H
#define LCN_HTTP_H

#include <stddef.h>

typedef struct {
    int    status_code;
    char  *body;
    size_t body_len;
    char  *headers;      /* raw headers string */
    size_t headers_len;
} LcnHttpResponse;

/* POST request with body and content type. Returns NULL on error. */
LcnHttpResponse *lcn_http_post(const char *url, const char *body, const char *content_type);

/* POST request with extra headers (e.g., "Authorization: Bearer <key>\r\n"). */
LcnHttpResponse *lcn_http_post_with_headers(const char *url, const char *body,
                                             const char *content_type,
                                             const char *extra_headers);

/* GET request. Returns NULL on error. */
LcnHttpResponse *lcn_http_get(const char *url);

/* Free response */
void lcn_http_response_free(LcnHttpResponse *resp);

/* Get a specific header value (case-insensitive). Returns NULL if not found. Caller must free(). */
char *lcn_http_get_header(const LcnHttpResponse *resp, const char *name);

#endif /* LCN_HTTP_H */
