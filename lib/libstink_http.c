/* Minimal userland HTTP/1.0 GET client. Built on top of sys_sock_* and the
 * DNS resolver -- no TLS, no chunked transfer encoding, no compression.
 * Plenty for downloading .stinkpkg packages from a static-file repo.
 *
 * URL syntax: "http://host[:port]/path" -- the parser drops the scheme
 * (only http:// supported), splits host[:port], and treats everything from
 * the first '/' onward as the request path. */
#include "libstink.h"
#include "libstink_http.h"

#define DEFAULT_PORT 80

/* Parse "http://host[:port]/path" into host[], port, path[]. Returns 0 on
 * success, -1 on malformed input. */
static int parse_url(const char *url, char *host, unsigned int host_cap,
                     unsigned short *port, char *path, unsigned int path_cap)
{
	const char *p = url;
	const char *scheme = "http://";
	for (int i = 0; scheme[i]; i++) {
		if (p[i] != scheme[i])
			return -1;
	}
	p += 7;

	unsigned int hi = 0;
	*port = DEFAULT_PORT;

	/* host (until ':' or '/' or end) */
	while (*p && *p != ':' && *p != '/') {
		if (hi + 1 >= host_cap)
			return -1;
		host[hi++] = *p++;
	}
	host[hi] = '\0';
	if (hi == 0)
		return -1;

	/* optional port */
	if (*p == ':') {
		p++;
		unsigned short v = 0;
		while (*p >= '0' && *p <= '9') {
			v = (unsigned short)(v * 10 + (*p - '0'));
			p++;
		}
		*port = v ? v : DEFAULT_PORT;
	}

	/* path */
	if (*p != '/') {
		path[0] = '/';
		path[1] = '\0';
	} else {
		unsigned int pi = 0;
		while (*p && pi + 1 < path_cap)
			path[pi++] = *p++;
		path[pi] = '\0';
	}
	return 0;
}

/* Block on DNS until the answer arrives or 'tries' poll cycles elapse.
 * Each cycle = one network frame processed + 10 ms sleep, so a tries of 200
 * works out to roughly 2 s of wall clock with a small DNS round-trip. */
static int wait_for_dns(unsigned int *ip_out, int tries)
{
	for (int i = 0; i < tries; i++) {
		sys_net_poll();
		if (sys_dns_poll(ip_out) == 1 && *ip_out != 0)
			return 0;
		sys_sleep_ms(10);
	}
	return -1;
}

/* Wait until the socket reaches the requested state. */
static int wait_for_state(int sock, int target, int tries)
{
	for (int i = 0; i < tries; i++) {
		sys_net_poll();
		if (sys_sock_state(sock) == target)
			return 0;
		if (sys_sock_state(sock) == SYS_TCP_CLOSED)
			return -1;
		sys_sleep_ms(10);
	}
	return -1;
}

/* Reads from the socket until 'cap' bytes are buffered or the connection
 * closes. Returns the byte count, -1 on error. */
static int slurp_response(int sock, unsigned char *buf, unsigned int cap)
{
	unsigned int total = 0;
	int idle = 0;

	while (total < cap) {
		int n = sys_sock_recv(sock, buf + total, cap - total);
		if (n < 0)
			return -1;
		if (n == 0) {
			sys_net_poll();
			if (sys_sock_state(sock) == SYS_TCP_CLOSE_WAIT ||
			    sys_sock_state(sock) == SYS_TCP_CLOSED)
				break;
			if (++idle > 500)        /* ~5 s of nothing -> bail */
				break;
			sys_sleep_ms(10);
			continue;
		}
		idle = 0;
		total += (unsigned int)n;
	}
	return (int)total;
}

/* Find the body start by scanning for the "\r\n\r\n" delimiter. Returns the
 * offset of the first body byte, or -1 if absent. */
static int find_body(const unsigned char *buf, unsigned int len)
{
	for (unsigned int i = 0; i + 3 < len; i++) {
		if (buf[i] == '\r' && buf[i+1] == '\n' &&
		    buf[i+2] == '\r' && buf[i+3] == '\n')
			return (int)i + 4;
	}
	return -1;
}

int http_get(const char *url, void *body_buf, unsigned int body_cap,
             int *status_out)
{
	if (!url || !body_buf || body_cap == 0)
		return -1;

	char           host[128];
	char           path[256];
	unsigned short port;
	if (parse_url(url, host, sizeof(host), &port, path, sizeof(path)) != 0)
		return -1;

	if (sys_dns_request(host) != 0)
		return -1;
	unsigned int ip = 0;
	if (wait_for_dns(&ip, 500) != 0)
		return -1;

	int sock = sys_sock_connect(ip, port);
	if (sock < 0)
		return -1;
	if (wait_for_state(sock, SYS_TCP_ESTABLISHED, 500) != 0) {
		sys_sock_close(sock);
		return -1;
	}

	/* Build "GET <path> HTTP/1.0\r\nHost: <host>\r\nConnection: close\r\n\r\n". */
	char req[512];
	int rl = snprintf(req, sizeof(req),
	                  "GET %s HTTP/1.0\r\nHost: %s\r\nConnection: close\r\n\r\n",
	                  path, host);
	if (rl <= 0 || (unsigned int)rl >= sizeof(req)) {
		sys_sock_close(sock);
		return -1;
	}
	sys_sock_send(sock, req, (unsigned int)rl);

	/* Pull the whole response. body_cap is the user's body buffer; we
	 * temporarily reuse it for header+body and then shift body forward
	 * after parsing. Header overhead is small in practice. */
	unsigned char *buf = (unsigned char *)body_buf;
	int got = slurp_response(sock, buf, body_cap);
	sys_sock_close(sock);
	if (got <= 0)
		return -1;

	int body_off = find_body(buf, (unsigned int)got);
	if (body_off < 0)
		return -1;

	/* Parse status code from line 1: "HTTP/1.x SSS ..." */
	if (status_out) {
		int sp = 0;
		while (sp < got && buf[sp] != ' ') sp++;
		if (sp + 4 < got) {
			*status_out = (buf[sp+1] - '0') * 100 +
			              (buf[sp+2] - '0') * 10  +
			              (buf[sp+3] - '0');
		} else {
			*status_out = 0;
		}
	}

	int body_len = got - body_off;
	for (int i = 0; i < body_len; i++)
		buf[i] = buf[body_off + i];
	return body_len;
}
