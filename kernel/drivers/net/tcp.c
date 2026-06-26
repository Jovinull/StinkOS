/* TCP implementation -- scaffold + connection setup / teardown.
 *
 * This first commit covers:
 *   - The TCB pool + 5-tuple lookup.
 *   - Segment build/send with the pseudo-header checksum.
 *   - tcp_connect (active open): allocate TCB, send SYN, drive SYN_SENT.
 *   - tcp_handle dispatch: route incoming segments to the right TCB.
 *   - SYN-SENT -> ESTABLISHED transition on a SYN-ACK reply, with the
 *     mandatory ACK back.
 *   - tcp_close: drives the FIN handshake from ESTABLISHED.
 *   - RST on segments that don't match a TCB.
 *
 * Not yet wired:
 *   - tcp_send / tcp_recv user data flow (send/receive buffers exist but
 *     aren't drained from the TCB into the wire yet).
 *   - Retransmit timer + RTO.
 *   - Window scaling, SACK, fast retransmit, congestion control.
 *   - Passive open (LISTEN) state machine.
 * Those land in subsequent commits without breaking this scaffold.
 */
#include "tcp.h"
#include "ipv4.h"
#include "ethernet.h"

#define TCP_MAX_CONNS    8
#define TCP_BUFFER_SIZE  4096
#define TCP_MSS          1460
#define TCP_DEFAULT_WIN  4096

#define EPHEMERAL_BASE   49152

struct tcb {
	enum tcp_state state;

	ipv4_t          local_ip;
	ipv4_t          remote_ip;
	unsigned short  local_port;     /* host order */
	unsigned short  remote_port;

	unsigned int    snd_una;        /* oldest unacked seq    */
	unsigned int    snd_nxt;        /* next seq to send      */
	unsigned int    snd_wnd;        /* peer-advertised window */
	unsigned int    rcv_nxt;        /* next seq we expect    */
	unsigned int    rcv_wnd;        /* our advertised window */

	unsigned char   rx_buf[TCP_BUFFER_SIZE];
	unsigned int    rx_head;        /* next byte to deliver to user */
	unsigned int    rx_tail;        /* next byte to fill            */

	unsigned char   tx_buf[TCP_BUFFER_SIZE];
	unsigned int    tx_head;        /* next byte to put on wire     */
	unsigned int    tx_tail;        /* next byte to append          */

	int             in_use;
};

static struct tcb conns[TCP_MAX_CONNS];
static unsigned short  next_ephemeral = EPHEMERAL_BASE;
static unsigned int    initial_seq    = 0x4B4E5453u;   /* "STNK" */

static unsigned int tx_pending(const struct tcb *t)
{
	return (t->tx_tail + TCP_BUFFER_SIZE - t->tx_head) % TCP_BUFFER_SIZE;
}

/* Pseudo-header for TCP checksum (RFC 793 section 3.1): prepended to the
 * segment for the one's-complement sum but not actually transmitted. */
struct pseudo_hdr {
	ipv4_t         src_ip;
	ipv4_t         dst_ip;
	unsigned char  zero;
	unsigned char  protocol;
	unsigned short tcp_length;       /* network byte order */
} __attribute__((packed));

static int tcb_alloc(void)
{
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		if (!conns[i].in_use) {
			struct tcb *t = &conns[i];
			t->in_use = 1;
			t->state  = TCP_CLOSED;
			t->rx_head = t->rx_tail = 0;
			t->tx_head = t->tx_tail = 0;
			t->rcv_wnd = TCP_BUFFER_SIZE;
			return i;
		}
	}
	return -1;
}

/* Find the TCB matching the segment's (remote_ip, remote_port, local_port).
 * Returns -1 if no connection matches (caller will RST). */
static int tcb_find(ipv4_t remote_ip, unsigned short remote_port,
                    unsigned short local_port)
{
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		struct tcb *t = &conns[i];
		if (!t->in_use)
			continue;
		if (t->remote_ip   == remote_ip   &&
		    t->remote_port == remote_port &&
		    t->local_port  == local_port)
			return i;
	}
	return -1;
}

/* Compute the TCP checksum (one's-complement over pseudo-header + segment).
 * 'segment' must hold the TCP header + any data, with the checksum field
 * already zeroed by the caller. */
static unsigned short tcp_checksum(ipv4_t src, ipv4_t dst,
                                   const void *segment, unsigned int seg_len)
{
	unsigned int sum = 0;
	const unsigned char *bytes;
	struct pseudo_hdr ph;
	ph.src_ip     = src;
	ph.dst_ip     = dst;
	ph.zero       = 0;
	ph.protocol   = IP_PROTO_TCP;
	ph.tcp_length = htons((unsigned short)seg_len);

	bytes = (const unsigned char *)&ph;
	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)bytes[i] << 8) | bytes[i + 1];

	bytes = (const unsigned char *)segment;
	for (unsigned int i = 0; i + 1 < seg_len; i += 2)
		sum += ((unsigned int)bytes[i] << 8) | bytes[i + 1];
	if (seg_len & 1)
		sum += (unsigned int)bytes[seg_len - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return htons((unsigned short)(~sum & 0xFFFFu));
}

/* Emit a TCP segment with the given flags, optional data payload. */
static void tcp_emit(struct tcb *t, unsigned char flags,
                     const void *data, unsigned int data_len)
{
	unsigned char  buf[1500];
	struct tcp_hdr *h = (struct tcp_hdr *)buf;
	if (sizeof(*h) + data_len > sizeof(buf))
		return;

	h->src_port = htons(t->local_port);
	h->dst_port = htons(t->remote_port);
	h->seq      = htonl(t->snd_nxt);
	h->ack      = htonl(t->rcv_nxt);
	h->data_off = (5u << 4);                /* 5 dwords = 20 bytes, no options */
	h->flags    = flags;
	h->window   = htons((unsigned short)t->rcv_wnd);
	h->checksum = 0;
	h->urg      = 0;

	const unsigned char *src = (const unsigned char *)data;
	for (unsigned int i = 0; i < data_len; i++)
		buf[sizeof(*h) + i] = src[i];

	h->checksum = tcp_checksum(t->local_ip, t->remote_ip,
	                           buf, sizeof(*h) + data_len);

	ipv4_send(t->remote_ip, IP_PROTO_TCP, buf, sizeof(*h) + data_len);
}

/* Push whatever is queued in the tx buffer onto the wire. Stop-and-wait:
 * only ships one MSS-sized segment at a time and waits for the ACK before
 * sending the next. This caps throughput but keeps the code tiny -- congestion
 * control + sliding window come in a follow-up commit. The bytes stay in
 * tx_buf until ACK'd so we can retransmit if needed. */
static void tcp_drain_tx(struct tcb *t)
{
	if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
		return;

	unsigned int in_flight = t->snd_nxt - t->snd_una;
	if (in_flight > 0)
		return;

	unsigned int avail = tx_pending(t);
	if (avail == 0)
		return;

	unsigned int chunk = avail;
	if (chunk > TCP_MSS)
		chunk = TCP_MSS;
	if (t->snd_wnd != 0 && chunk > t->snd_wnd)
		chunk = t->snd_wnd;

	/* Copy bytes out of the wrapping tx ring into a linear scratch buffer
	 * for emit -- the segment builder wants contiguous data. */
	unsigned char linear[TCP_MSS];
	for (unsigned int i = 0; i < chunk; i++)
		linear[i] = t->tx_buf[(t->tx_head + i) % TCP_BUFFER_SIZE];

	tcp_emit(t, TCP_ACK | TCP_PSH, linear, chunk);
	t->snd_nxt += chunk;
}

/* Bare RST to an unsolicited segment (RFC 793 reset generation). */
static void tcp_emit_rst(ipv4_t src_ip, const struct tcp_hdr *in_hdr)
{
	struct tcb scratch;
	scratch.local_ip    = net_get_local_ip();
	scratch.remote_ip   = src_ip;
	scratch.local_port  = ntohs(in_hdr->dst_port);
	scratch.remote_port = ntohs(in_hdr->src_port);
	scratch.snd_nxt     = (in_hdr->flags & TCP_ACK) ? ntohl(in_hdr->ack) : 0;
	scratch.rcv_nxt     = ntohl(in_hdr->seq) +
	                      ((in_hdr->flags & TCP_SYN) ? 1u : 0u);
	scratch.rcv_wnd     = 0;
	tcp_emit(&scratch, TCP_RST | TCP_ACK, (void *)0, 0);
}

tcp_handle_t tcp_connect(ipv4_t dst_ip, unsigned short dst_port)
{
	int idx = tcb_alloc();
	if (idx < 0)
		return -1;
	struct tcb *t = &conns[idx];
	t->local_ip    = net_get_local_ip();
	t->remote_ip   = dst_ip;
	t->local_port  = next_ephemeral++;
	if (next_ephemeral == 0)
		next_ephemeral = EPHEMERAL_BASE;
	t->remote_port = dst_port;
	t->snd_una     = initial_seq;
	t->snd_nxt     = initial_seq;
	t->rcv_nxt     = 0;
	initial_seq   += 64000u;       /* coarse ISN bump, avoids reuse */

	t->state = TCP_SYN_SENT;
	tcp_emit(t, TCP_SYN, (void *)0, 0);
	t->snd_nxt += 1;                /* SYN consumes one sequence slot */
	return idx;
}

tcp_handle_t tcp_listen(unsigned short local_port)
{
	int idx = tcb_alloc();
	if (idx < 0)
		return -1;
	struct tcb *t = &conns[idx];
	t->local_ip    = net_get_local_ip();
	t->remote_ip   = 0;
	t->local_port  = local_port;
	t->remote_port = 0;
	t->state       = TCP_LISTEN;
	return idx;
}

int tcp_send(tcp_handle_t h, const void *buf, unsigned int len)
{
	if (h < 0 || h >= TCP_MAX_CONNS || !conns[h].in_use)
		return -1;
	struct tcb *t = &conns[h];
	if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
		return -1;

	const unsigned char *src = (const unsigned char *)buf;
	unsigned int put = 0;
	while (put < len) {
		unsigned int next = (t->tx_tail + 1) % TCP_BUFFER_SIZE;
		if (next == t->tx_head)
			break;            /* buffer full */
		t->tx_buf[t->tx_tail] = src[put++];
		t->tx_tail = next;
	}
	tcp_drain_tx(t);          /* try to push it now */
	return (int)put;
}

int tcp_recv(tcp_handle_t h, void *buf, unsigned int max)
{
	if (h < 0 || h >= TCP_MAX_CONNS || !conns[h].in_use)
		return -1;
	struct tcb *t = &conns[h];

	unsigned char *dst = (unsigned char *)buf;
	unsigned int got = 0;
	while (got < max && t->rx_head != t->rx_tail) {
		dst[got++] = t->rx_buf[t->rx_head];
		t->rx_head = (t->rx_head + 1) % TCP_BUFFER_SIZE;
	}
	return (int)got;
}

void tcp_close(tcp_handle_t h)
{
	if (h < 0 || h >= TCP_MAX_CONNS || !conns[h].in_use)
		return;
	struct tcb *t = &conns[h];

	switch (t->state) {
	case TCP_ESTABLISHED:
		t->state = TCP_FIN_WAIT_1;
		tcp_emit(t, TCP_FIN | TCP_ACK, (void *)0, 0);
		t->snd_nxt += 1;
		break;
	case TCP_CLOSE_WAIT:
		t->state = TCP_LAST_ACK;
		tcp_emit(t, TCP_FIN | TCP_ACK, (void *)0, 0);
		t->snd_nxt += 1;
		break;
	default:
		t->state  = TCP_CLOSED;
		t->in_use = 0;
		break;
	}
}

enum tcp_state tcp_get_state(tcp_handle_t h)
{
	if (h < 0 || h >= TCP_MAX_CONNS || !conns[h].in_use)
		return TCP_CLOSED;
	return conns[h].state;
}

void tcp_handle(const void *payload, unsigned int len, ipv4_t src_ip)
{
	if (len < sizeof(struct tcp_hdr))
		return;
	const struct tcp_hdr *h = (const struct tcp_hdr *)payload;

	unsigned short src_port = ntohs(h->src_port);
	unsigned short dst_port = ntohs(h->dst_port);

	int idx = tcb_find(src_ip, src_port, dst_port);
	if (idx < 0) {
		if (!(h->flags & TCP_RST))
			tcp_emit_rst(src_ip, h);
		return;
	}
	struct tcb *t = &conns[idx];

	unsigned int seg_seq = ntohl(h->seq);
	unsigned int seg_ack = ntohl(h->ack);

	if (h->flags & TCP_RST) {
		t->state  = TCP_CLOSED;
		t->in_use = 0;
		return;
	}

	switch (t->state) {
	case TCP_SYN_SENT:
		if ((h->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    seg_ack == t->snd_nxt) {
			t->rcv_nxt = seg_seq + 1;
			t->snd_una = seg_ack;
			t->snd_wnd = ntohs(h->window);
			t->state   = TCP_ESTABLISHED;
			tcp_emit(t, TCP_ACK, (void *)0, 0);
		}
		break;

	case TCP_ESTABLISHED:
	case TCP_CLOSE_WAIT: {
		/* Advance snd_una if the ACK covers new ground, then free that
		 * many bytes from the tx ring so the next drain can move on. */
		if (h->flags & TCP_ACK) {
			unsigned int acked = seg_ack - t->snd_una;
			unsigned int inflight = t->snd_nxt - t->snd_una;
			if (acked > 0 && acked <= inflight) {
				t->snd_una = seg_ack;
				/* Trim acked bytes off the head of tx_buf. */
				t->tx_head = (t->tx_head + acked) % TCP_BUFFER_SIZE;
				t->snd_wnd = ntohs(h->window);
				tcp_drain_tx(t);
			}
		}

		/* Take any in-order payload into rx_buf and ACK back. */
		unsigned int data_off = (h->data_off >> 4) * 4u;
		if (data_off >= len)
			data_off = sizeof(struct tcp_hdr);
		unsigned int data_len = len - data_off;
		if (data_len > 0 && seg_seq == t->rcv_nxt) {
			const unsigned char *data =
			    (const unsigned char *)payload + data_off;
			unsigned int put = 0;
			while (put < data_len) {
				unsigned int next = (t->rx_tail + 1) % TCP_BUFFER_SIZE;
				if (next == t->rx_head)
					break;       /* rx buffer full */
				t->rx_buf[t->rx_tail] = data[put++];
				t->rx_tail = next;
			}
			t->rcv_nxt += put;
			tcp_emit(t, TCP_ACK, (void *)0, 0);
		}

		if ((h->flags & TCP_FIN) && t->state == TCP_ESTABLISHED) {
			t->rcv_nxt = seg_seq + data_len + 1;
			t->state   = TCP_CLOSE_WAIT;
			tcp_emit(t, TCP_ACK, (void *)0, 0);
		}
		break;
	}

	case TCP_FIN_WAIT_1:
		if (h->flags & TCP_ACK)
			t->state = TCP_FIN_WAIT_2;
		if (h->flags & TCP_FIN) {
			t->rcv_nxt = seg_seq + 1;
			t->state = TCP_TIME_WAIT;
			tcp_emit(t, TCP_ACK, (void *)0, 0);
		}
		break;

	case TCP_FIN_WAIT_2:
		if (h->flags & TCP_FIN) {
			t->rcv_nxt = seg_seq + 1;
			t->state = TCP_TIME_WAIT;
			tcp_emit(t, TCP_ACK, (void *)0, 0);
		}
		break;

	case TCP_LAST_ACK:
		if (h->flags & TCP_ACK) {
			t->state  = TCP_CLOSED;
			t->in_use = 0;
		}
		break;

	case TCP_TIME_WAIT:
		/* Quietly absorb retransmits; real implementation would arm a
		 * 2*MSL timer here. */
		break;

	default:
		break;
	}
}
