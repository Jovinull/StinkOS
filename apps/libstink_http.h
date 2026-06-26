/* Userland HTTP/1.0 GET client. Tiny: no TLS, no chunked transfer, no
 * keepalive, no redirects. Synchronous (the caller blocks). Designed for
 * static-file repositories like the .stinkpkg server. */
#ifndef LIBSTINK_HTTP_H
#define LIBSTINK_HTTP_H

/* Fetch 'url' (only http:// supported) and put the response body into
 * 'body_buf' (truncated to 'body_cap'). Returns the body byte count on
 * success or -1 on any failure (DNS, connect, parse). Optional
 * 'status_out' receives the HTTP status code (200, 404, ...). */
int http_get(const char *url, void *body_buf, unsigned int body_cap,
             int *status_out);

#endif
