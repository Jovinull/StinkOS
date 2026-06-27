/* Minimal BSD-style socket API over the existing sys_sock_* int 0x80 layer.
 *
 * The kernel speaks "TCB handle in / TCB handle out". This wrapper gives
 * userland POSIX-shaped names so ported code (curl-flavoured downloaders,
 * netcat-style snippets) does not have to learn the StinkOS-specific surface.
 * The implementation lives in lib/libstink_socket.c.
 *
 * Limits today:
 *   - AF_INET + SOCK_STREAM only (TCP/IPv4). Anything else returns -1.
 *   - 8 concurrent sockets per process (matches the kernel TCB pool).
 *   - bind() + listen() + accept() trail behind connect(): the kernel knows
 *     how to LISTEN but the wrapper currently exposes only the client path.
 *     Server-side helpers land in a follow-up commit.
 */
#ifndef LIBSTINK_SOCKET_H
#define LIBSTINK_SOCKET_H

#define AF_INET       2
#define SOCK_STREAM   1
#define IPPROTO_TCP   6
#define INADDR_ANY    0u

typedef unsigned short  sa_family_t;
typedef unsigned int    in_addr_t;
typedef unsigned short  in_port_t;
typedef unsigned int    socklen_t;

struct in_addr {
	in_addr_t  s_addr;             /* network byte order */
};

struct sockaddr_in {
	sa_family_t     sin_family;    /* AF_INET */
	in_port_t       sin_port;      /* network byte order */
	struct in_addr  sin_addr;
	unsigned char   sin_zero[8];   /* padding, ignored */
};

struct sockaddr {
	sa_family_t  sa_family;
	char         sa_data[14];
};

/* Allocate a new socket slot. Returns a non-negative descriptor on success
 * (0..7), -1 on failure. The descriptor is *not* the kernel handle -- it
 * indirects through a small per-process table. */
int   socket(int domain, int type, int protocol);

/* Establish a TCP connection through socket `sockfd` to the remote endpoint
 * described by `addr`. Returns 0 on success, -1 on bad descriptor or
 * unsupported family. The kernel handle is filled in transparently. */
int   connect(int sockfd, const struct sockaddr *addr, socklen_t addrlen);

/* Same semantics as POSIX -- count returned is bytes actually written /
 * read; -1 on error. flags is accepted but ignored. */
int   send(int sockfd, const void *buf, unsigned int len, int flags);
int   recv(int sockfd, void *buf, unsigned int len, int flags);

/* Tear down the connection and release the slot. */
int   closesocket(int sockfd);

/* Host -> network byte order helpers. Mirror BSD's htons / htonl /
 * inet_addr exactly so ported code that uses them keeps working. */
unsigned short htons_b(unsigned short v);
unsigned int   htonl_b(unsigned int v);

/* Convert dotted-quad ASCII ("192.168.1.10") to a network-byte-order u32.
 * Returns 0 (which is also a valid address) on parse failure -- callers
 * should know whether the empty string is acceptable for their context. */
in_addr_t inet_addr(const char *cp);

#endif
