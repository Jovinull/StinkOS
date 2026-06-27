/* BSD-style socket wrapper over sys_sock_*. See libstink_socket.h. */
#include "libstink.h"
#include "libstink_socket.h"

#define SOCKET_MAX  8

struct sock_slot {
	int            in_use;
	int            kernel_handle;   /* -1 until connect/listen sets it */
	unsigned short bound_port;       /* set by bind(), used by listen() */
	int            is_listener;      /* 1 once listen() succeeds */
};

static struct sock_slot table[SOCKET_MAX];

static struct sock_slot *slot(int sockfd)
{
	if (sockfd < 0 || sockfd >= SOCKET_MAX)
		return (struct sock_slot *)0;
	struct sock_slot *s = &table[sockfd];
	return s->in_use ? s : (struct sock_slot *)0;
}

int socket(int domain, int type, int protocol)
{
	if (domain != AF_INET)                            return -1;
	if (type   != SOCK_STREAM)                        return -1;
	if (protocol != 0 && protocol != IPPROTO_TCP)     return -1;

	for (int i = 0; i < SOCKET_MAX; i++) {
		if (!table[i].in_use) {
			table[i].in_use        = 1;
			table[i].kernel_handle = -1;
			table[i].bound_port    = 0;
			table[i].is_listener   = 0;
			return i;
		}
	}
	return -1;
}

int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	struct sock_slot *s = slot(sockfd);
	if (!s)
		return -1;
	if (!addr || addrlen < sizeof(struct sockaddr_in))
		return -1;
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	if (sin->sin_family != AF_INET)
		return -1;
	/* sin_port is network byte order per BSD; the kernel expects host. */
	s->bound_port = (unsigned short)
		(((sin->sin_port & 0x00FF) << 8) | ((sin->sin_port & 0xFF00) >> 8));
	return 0;
}

int listen(int sockfd, int backlog)
{
	(void)backlog;                                   /* kernel ignores it today */
	struct sock_slot *s = slot(sockfd);
	if (!s || s->bound_port == 0)
		return -1;
	int kh = sys_sock_listen(s->bound_port);
	if (kh < 0)
		return -1;
	s->kernel_handle = kh;
	s->is_listener   = 1;
	return 0;
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
	struct sock_slot *s = slot(sockfd);
	if (!s || !s->is_listener || s->kernel_handle < 0)
		return -1;

	/* Block until the kernel TCB leaves LISTEN. The transition lands when
	 * a peer SYN is processed -- sys_net_poll between checks gives the RX
	 * path a chance to fire while we sleep. */
	for (;;) {
		int st = sys_sock_state(s->kernel_handle);
		if (st != SYS_TCP_LISTEN && st != SYS_TCP_SYN_RECEIVED)
			break;
		sys_net_poll();
		sys_sleep_ms(10);
	}

	/* Allocate a fresh slot that aliases the same kernel TCB. The original
	 * listener slot is left in a "consumed" state -- close it to free the
	 * kernel handle, or accept again only after another listen(). */
	for (int i = 0; i < SOCKET_MAX; i++) {
		if (!table[i].in_use) {
			table[i].in_use        = 1;
			table[i].kernel_handle = s->kernel_handle;
			table[i].bound_port    = s->bound_port;
			table[i].is_listener   = 0;
			s->is_listener         = 0;
			s->kernel_handle       = -1;
			if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_in)) {
				struct sockaddr_in *out = (struct sockaddr_in *)addr;
				out->sin_family    = AF_INET;
				out->sin_port      = 0;            /* remote port not exposed */
				out->sin_addr.s_addr = 0;          /* remote ip   not exposed */
				for (int k = 0; k < 8; k++) out->sin_zero[k] = 0;
				*addrlen = sizeof(struct sockaddr_in);
			}
			return i;
		}
	}
	return -1;
}

int connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
	struct sock_slot *s = slot(sockfd);
	if (!s)
		return -1;
	if (!addr || addrlen < sizeof(struct sockaddr_in))
		return -1;
	const struct sockaddr_in *sin = (const struct sockaddr_in *)addr;
	if (sin->sin_family != AF_INET)
		return -1;

	/* The kernel expects the port in host order; sin_port is network order
	 * per BSD convention, so swap it before the syscall. */
	unsigned short port_host = (unsigned short)
		(((sin->sin_port & 0x00FF) << 8) | ((sin->sin_port & 0xFF00) >> 8));

	int kh = sys_sock_connect(sin->sin_addr.s_addr, port_host);
	if (kh < 0)
		return -1;
	s->kernel_handle = kh;
	return 0;
}

int send(int sockfd, const void *buf, unsigned int len, int flags)
{
	(void)flags;
	struct sock_slot *s = slot(sockfd);
	if (!s || s->kernel_handle < 0)
		return -1;
	return sys_sock_send(s->kernel_handle, buf, len);
}

int recv(int sockfd, void *buf, unsigned int len, int flags)
{
	(void)flags;
	struct sock_slot *s = slot(sockfd);
	if (!s || s->kernel_handle < 0)
		return -1;
	return sys_sock_recv(s->kernel_handle, buf, len);
}

int closesocket(int sockfd)
{
	struct sock_slot *s = slot(sockfd);
	if (!s)
		return -1;
	if (s->kernel_handle >= 0)
		sys_sock_close(s->kernel_handle);
	s->in_use        = 0;
	s->kernel_handle = -1;
	return 0;
}

unsigned short htons_b(unsigned short v)
{
	return (unsigned short)((v << 8) | (v >> 8));
}

unsigned int htonl_b(unsigned int v)
{
	return ((v & 0x000000FFu) << 24) |
	       ((v & 0x0000FF00u) <<  8) |
	       ((v & 0x00FF0000u) >>  8) |
	       ((v & 0xFF000000u) >> 24);
}

in_addr_t inet_addr(const char *cp)
{
	if (!cp)
		return 0;
	unsigned int parts[4] = {0, 0, 0, 0};
	int pi = 0, digits = 0;
	for (const char *p = cp; ; p++) {
		if (*p >= '0' && *p <= '9') {
			parts[pi] = parts[pi] * 10u + (unsigned int)(*p - '0');
			if (parts[pi] > 255) return 0;
			digits++;
		} else if (*p == '.' || *p == '\0') {
			if (digits == 0) return 0;
			if (*p == '\0') {
				if (pi != 3) return 0;
				break;
			}
			pi++;
			if (pi > 3) return 0;
			digits = 0;
		} else {
			return 0;
		}
	}
	return ((parts[3] & 0xFFu) << 24) |
	       ((parts[2] & 0xFFu) << 16) |
	       ((parts[1] & 0xFFu) <<  8) |
	        (parts[0] & 0xFFu);
}
