/* Transmission Control Protocol (RFC 793) -- minimal in-kernel implementation.
 * This header pins down the wire structure, state enum, and public API used
 * by the socket-syscall layer. The .c file holds the TCB pool, state machine
 * and segment send/receive paths.
 *
 * Limits today:
 *   - Up to 8 concurrent connections (TCP_MAX_CONNS).
 *   - 4 KiB per direction of buffered data per connection.
 *   - Single-packet retransmission (no SACK).
 *   - MSS fixed to 1460 bytes.
 *   - No congestion control beyond a single in-flight segment per RTT.
 * Plenty for HTTP-style request/response from the package manager; can grow
 * incrementally without breaking callers. */
#ifndef TCP_H
#define TCP_H

#include "net.h"

/* TCP header: 20 bytes when there are no options. */
struct tcp_hdr {
	unsigned short src_port;        /* network byte order */
	unsigned short dst_port;
	unsigned int   seq;             /* network byte order */
	unsigned int   ack;
	unsigned char  data_off;        /* high 4 bits: data offset in dwords */
	unsigned char  flags;
	unsigned short window;
	unsigned short checksum;
	unsigned short urg;
} __attribute__((packed));

/* TCP flag bits (in tcp_hdr.flags). */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

enum tcp_state {
	TCP_CLOSED       = 0,
	TCP_LISTEN       = 1,
	TCP_SYN_SENT     = 2,
	TCP_SYN_RECEIVED = 3,
	TCP_ESTABLISHED  = 4,
	TCP_FIN_WAIT_1   = 5,
	TCP_FIN_WAIT_2   = 6,
	TCP_CLOSE_WAIT   = 7,
	TCP_CLOSING      = 8,
	TCP_LAST_ACK     = 9,
	TCP_TIME_WAIT    = 10,
};

/* Opaque handle into the kernel TCB pool. Returned by tcp_connect/listen;
 * passed to tcp_send/recv/close. Values 0..TCP_MAX_CONNS-1 on success,
 * -1 on failure. */
typedef int tcp_handle_t;

/* Active open: SYN to (dst_ip, dst_port) from an ephemeral local port. */
tcp_handle_t tcp_connect(ipv4_t dst_ip, unsigned short dst_port);

/* Passive open: bind to local_port, wait for incoming SYN. */
tcp_handle_t tcp_listen(unsigned short local_port);

/* Block-ish send: appends 'len' bytes to the send buffer. Returns the bytes
 * queued (could be < len if buffer is full); -1 on bad handle / closed. */
int tcp_send(tcp_handle_t h, const void *buf, unsigned int len);

/* Non-blocking receive: copies up to 'max' bytes from the receive buffer.
 * Returns the byte count, 0 if no data is pending. */
int tcp_recv(tcp_handle_t h, void *buf, unsigned int max);

/* Initiates FIN handshake. After this the handle still produces queued recv
 * data until the peer's FIN; then it transitions to CLOSED and the slot is
 * freed. */
void tcp_close(tcp_handle_t h);

/* Reap every TCB owned by `pid`. Used by process teardown to recover
 * connection slots that the dying app never closed itself. */
void tcp_close_pid(int pid);

/* Current state of the connection (for debugging / waits). */
enum tcp_state tcp_get_state(tcp_handle_t h);

/* Dispatched from ipv4.c on IP_PROTO_TCP frames. */
void tcp_handle(const void *payload, unsigned int len, ipv4_t src_ip);

/* Periodic retransmission timer. Walks every in-use TCB and re-sends the
 * oldest unacked segment if its RTO has elapsed. Exponential backoff doubles
 * the RTO on each retry; after TCP_MAX_RETRIES the connection is dropped.
 * Called from net_poll_once() so it runs whenever the kernel pumps the NIC. */
void tcp_tick(void);

/* Read-only snapshot of one TCB slot. Powers the userland netstat tool;
 * unused slots return state=TCP_CLOSED and zero everywhere else. */
struct tcp_info {
	unsigned int    state;         /* enum tcp_state, widened to u32 */
	unsigned int    local_ip;      /* network byte order */
	unsigned int    remote_ip;     /* network byte order */
	unsigned short  local_port;    /* host order */
	unsigned short  remote_port;
	unsigned int    rx_pending;    /* bytes ready for tcp_recv */
	unsigned int    tx_pending;    /* bytes queued for send (unsent+unacked) */
	unsigned int    in_use;        /* 1 = slot active, 0 = free */
};

/* Fill `out` with the snapshot of slot `idx` (0..7). Returns 0 on success,
 * -1 if idx is out of range or out is NULL. */
int tcp_get_info(int idx, struct tcp_info *out);

#endif
