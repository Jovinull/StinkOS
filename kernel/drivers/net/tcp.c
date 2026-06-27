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
#include "interrupts.h"   /* pit_ticks() for the RTO timer */

#define TCP_MAX_CONNS    8
#define TCP_BUFFER_SIZE  4096
#define TCP_MSS          1460
#define TCP_DEFAULT_WIN  4096

#define EPHEMERAL_BASE   49152

/* Retransmission constants. PIT runs at 100 Hz, so one "tick" is 10 ms.
 *   - TCP_RTO_INITIAL  1000 ms (100 ticks): a conservative starting RTT
 *     for a LAN where round-trips are sub-ms but we want headroom before
 *     spamming the wire.
 *   - TCP_RTO_MAX      60000 ms: ceiling after exponential backoff.
 *   - TCP_MAX_RETRIES  5: ~31 s of total wait at exp backoff before drop. */
#define TCP_RTO_INITIAL  100
#define TCP_RTO_MAX      6000
#define TCP_MAX_RETRIES  5

/* Keepalive: after KEEPALIVE_IDLE ticks of no incoming data on an
 * ESTABLISHED connection, send one probe every KEEPALIVE_INTERVAL ticks.
 * Peer's ACK refreshes last_activity_ticks, suppressing further probes
 * until the next idle window. */
#define TCP_KEEPALIVE_IDLE      7200       /* 72 s */
#define TCP_KEEPALIVE_INTERVAL  3000       /* 30 s between probes */

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

	/* Retransmission state. last_seg_ticks = PIT tick when the oldest
	 * unacked segment was sent (or 0 if nothing is in flight). rto_ticks =
	 * current timeout, doubled on every retry up to TCP_RTO_MAX. retries =
	 * how many times the current segment has been resent; the connection
	 * gets dropped after TCP_MAX_RETRIES consecutive failures. */
	unsigned int    last_seg_ticks;
	unsigned int    rto_ticks;
	unsigned int    retries;

	/* Congestion control (Reno-ish, no fast retransmit). cwnd caps
	 * simultaneous bytes in flight; slow start doubles cwnd per round trip
	 * until it crosses ssthresh, then we switch to additive increase. A
	 * retransmission timeout halves ssthresh and resets cwnd to one MSS. */
	unsigned int    cwnd;
	unsigned int    ssthresh;

	/* Peer's advertised window-scale shift (RFC 7323). Set when a SYN /
	 * SYN-ACK carries TCP option kind=3; zero means no scaling. We apply
	 * it on every snd_wnd update so the kernel sees the real byte count
	 * the peer is willing to buffer. Our own receive window stays small
	 * enough to fit in the raw 16-bit field, so we never emit kind=3. */
	unsigned char   peer_wscale;

	/* Maximum segment size the peer announced via TCP option kind=2.
	 * Clamps our outbound chunk size in tcp_drain_tx; defaults to TCP_MSS
	 * (matches the legacy 536-byte minimum RFC 879 path-MTU when the
	 * peer omits the option). */
	unsigned short  peer_mss;

	/* Keepalive: last_activity_ticks bumps on every RX; last_keepalive_ticks
	 * paces the probes themselves. Both unused (zero) until we've seen the
	 * first byte from the peer in ESTABLISHED. */
	unsigned int    last_activity_ticks;
	unsigned int    last_keepalive_ticks;

	/* Out-of-order receive queue. A single dropped packet on a steady
	 * stream typically causes the next two packets to arrive ahead of the
	 * retransmit; cache them here so they don't need to be retransmitted
	 * themselves, then drain into rx_buf once rcv_nxt catches up. */
	struct {
		int           used;
		unsigned int  seq;
		unsigned int  len;
		unsigned char buf[TCP_MSS];
	} ooo[2];

	/* Peer-side SACK scoreboard (RFC 2018, sender-side use): the lowest
	 * left edge of any SACK block in the most recent ACK. Zero means the
	 * peer reported no SACKed ranges (or none ahead of snd_una), so the
	 * retransmit path falls back to plain Reno. When non-zero AND ahead
	 * of snd_una, tcp_retransmit clamps its chunk so we never resend
	 * bytes the peer has already acknowledged out-of-order. Only the
	 * lowest left edge is tracked -- that's the gap we MUST fill. */
	unsigned int    peer_sack_lo;

	/* Fast retransmit (RFC 5681 §3.2): count consecutive duplicate ACKs.
	 * A "dup ACK" is an ACK that does not advance snd_una, carries no
	 * payload, and does not shrink/grow the peer window. After 3 dups in
	 * a row, retransmit the oldest unacked segment immediately instead of
	 * waiting for the RTO to expire, and tighten ssthresh + cwnd to half
	 * the in-flight ceiling so we don't re-flood the path that just
	 * dropped data. Cleared on any forward ACK. */
	unsigned char   dup_acks;

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
			t->last_seg_ticks = 0;
			t->rto_ticks      = TCP_RTO_INITIAL;
			t->retries        = 0;
			t->cwnd           = 2u * TCP_MSS;     /* IW=2 segments */
			t->ssthresh       = 64u * TCP_MSS;    /* high; first loss tightens it */
			t->peer_wscale    = 0;
			t->peer_mss       = TCP_MSS;
			t->last_activity_ticks  = 0;
			t->last_keepalive_ticks = 0;
			t->dup_acks             = 0;
			t->peer_sack_lo         = 0;
			for (int k = 0; k < 2; k++) t->ooo[k].used = 0;
			return i;
		}
	}
	return -1;
}

/* Copy `data` (`n` bytes) into the rx ring, drop tail if it overflows; advance
 * rcv_nxt past the bytes actually buffered. Returns the count buffered. */
static unsigned int tcb_rx_inline(struct tcb *t, const unsigned char *data,
                                  unsigned int n)
{
	unsigned int put = 0;
	while (put < n) {
		unsigned int next = (t->rx_tail + 1) % TCP_BUFFER_SIZE;
		if (next == t->rx_head)
			break;
		t->rx_buf[t->rx_tail] = data[put++];
		t->rx_tail = next;
	}
	t->rcv_nxt += put;
	return put;
}

/* Park an out-of-order segment in a free slot. Drops it if both slots are
 * busy or the segment is larger than one MSS; both are tolerable losses --
 * the sender will retransmit and we will get it again in order. */
static void tcb_ooo_park(struct tcb *t, unsigned int seq,
                         const unsigned char *data, unsigned int n)
{
	if (n == 0 || n > TCP_MSS)
		return;
	for (int k = 0; k < 2; k++) {
		if (t->ooo[k].used && t->ooo[k].seq == seq)
			return;                                  /* already queued */
	}
	for (int k = 0; k < 2; k++) {
		if (t->ooo[k].used)
			continue;
		t->ooo[k].seq = seq;
		t->ooo[k].len = n;
		for (unsigned int i = 0; i < n; i++)
			t->ooo[k].buf[i] = data[i];
		t->ooo[k].used = 1;
		return;
	}
}

/* Drain any OOO slot whose seq has caught up with rcv_nxt; loop because
 * one absorbed segment can chain into another. */
static void tcb_ooo_drain(struct tcb *t)
{
	int progress = 1;
	while (progress) {
		progress = 0;
		for (int k = 0; k < 2; k++) {
			if (!t->ooo[k].used)
				continue;
			if (t->ooo[k].seq != t->rcv_nxt)
				continue;
			tcb_rx_inline(t, t->ooo[k].buf, t->ooo[k].len);
			t->ooo[k].used = 0;
			progress = 1;
		}
	}
}

/* Mark `t` as having just sent a retransmittable segment so the RTO timer
 * starts counting. Resets retries because the most recent emit was a fresh
 * transmit, not a retransmit. */
static void tcp_arm_rto(struct tcb *t)
{
	t->last_seg_ticks = pit_ticks();
	t->rto_ticks      = TCP_RTO_INITIAL;
	t->retries        = 0;
}

/* Re-emit the oldest unacked segment without advancing snd_nxt. Picks what to
 * resend based on the connection state: SYN in SYN_SENT, the in-flight data
 * chunk in ESTABLISHED/CLOSE_WAIT, FIN in FIN_WAIT_1 / LAST_ACK. */
static void tcp_retransmit(struct tcb *t)
{
	unsigned int saved = t->snd_nxt;
	t->snd_nxt = t->snd_una;           /* tcp_emit reads snd_nxt for h->seq */

	switch (t->state) {
	case TCP_SYN_SENT:
		tcp_emit_syn(t, TCP_SYN);
		break;
	case TCP_FIN_WAIT_1:
	case TCP_LAST_ACK:
		tcp_emit(t, TCP_FIN | TCP_ACK, (void *)0, 0);
		break;
	case TCP_ESTABLISHED:
	case TCP_CLOSE_WAIT: {
		unsigned int chunk = saved - t->snd_una;
		if (chunk > TCP_MSS)
			chunk = TCP_MSS;
		/* SACK sender-side: if the peer has SACKed bytes ahead of us,
		 * clamp the retransmit so it stops at the lowest sacked left
		 * edge -- peer already has the rest. peer_sack_lo is only set
		 * when it lies strictly above snd_una (see ACK handler). */
		if (t->peer_sack_lo != 0) {
			unsigned int gap = t->peer_sack_lo - t->snd_una;
			if (gap > 0 && chunk > gap)
				chunk = gap;
		}
		unsigned char linear[TCP_MSS];
		for (unsigned int i = 0; i < chunk; i++)
			linear[i] = t->tx_buf[(t->tx_head + i) % TCP_BUFFER_SIZE];
		tcp_emit(t, TCP_ACK | TCP_PSH, linear, chunk);
		break;
	}
	default:
		break;
	}

	t->snd_nxt = saved;
}

/* Find the TCB matching the segment's (remote_ip, remote_port, local_port).
 * An exact 5-tuple match wins; failing that, fall through to a LISTEN socket
 * on the same local_port (its remote fields are zeroed until it accepts a
 * SYN). Returns -1 if nothing matches (caller will RST). */
static int tcb_find(ipv4_t remote_ip, unsigned short remote_port,
                    unsigned short local_port)
{
	int listen_idx = -1;
	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		struct tcb *t = &conns[i];
		if (!t->in_use)
			continue;
		if (t->remote_ip   == remote_ip   &&
		    t->remote_port == remote_port &&
		    t->local_port  == local_port)
			return i;
		if (t->state == TCP_LISTEN && t->local_port == local_port)
			listen_idx = i;
	}
	return listen_idx;
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

/* Push whatever is queued in the tx buffer onto the wire. Sends up to
 * (cwnd - in_flight) bytes, in MSS-sized segments, bounded by the peer's
 * advertised window. Loops until either the tx ring is empty, cwnd is full,
 * or the peer window is exhausted. Each emitted segment arms the RTO if
 * none is armed yet, so a loss anywhere in the burst restarts the timer. */
static void tcp_drain_tx(struct tcb *t)
{
	if (t->state != TCP_ESTABLISHED && t->state != TCP_CLOSE_WAIT)
		return;

	for (;;) {
		unsigned int in_flight = t->snd_nxt - t->snd_una;
		unsigned int avail     = tx_pending(t);
		if (avail == 0)
			return;

		unsigned int window = (t->snd_wnd != 0) ? t->snd_wnd : avail;
		unsigned int budget = (t->cwnd > in_flight) ? (t->cwnd - in_flight) : 0;
		if (budget == 0)
			return;
		if (window <= in_flight)
			return;
		unsigned int peer_room = window - in_flight;
		if (budget > peer_room)
			budget = peer_room;

		unsigned int chunk = avail;
		unsigned int cap   = t->peer_mss ? t->peer_mss : TCP_MSS;
		if (chunk > cap)    chunk = cap;
		if (chunk > budget) chunk = budget;
		if (chunk == 0)
			return;

		unsigned char linear[TCP_MSS];
		for (unsigned int i = 0; i < chunk; i++)
			linear[i] = t->tx_buf[(t->tx_head + i) % TCP_BUFFER_SIZE];

		tcp_emit(t, TCP_ACK | TCP_PSH, linear, chunk);
		t->snd_nxt += chunk;
		if (t->last_seg_ticks == 0)
			tcp_arm_rto(t);
	}
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
	tcp_emit_syn(t, TCP_SYN);
	t->snd_nxt += 1;                /* SYN consumes one sequence slot */
	tcp_arm_rto(t);
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
		tcp_arm_rto(t);
		break;
	case TCP_CLOSE_WAIT:
		t->state = TCP_LAST_ACK;
		tcp_emit(t, TCP_FIN | TCP_ACK, (void *)0, 0);
		t->snd_nxt += 1;
		tcp_arm_rto(t);
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

/* Emit a SYN (or SYN-ACK) carrying our MSS option so the peer caps its
 * outbound chunks at our buffer's reach. Option layout: kind=2 len=4 +
 * mss(2). Padded with two NOPs to keep data_off aligned at 6 dwords.
 * Used by tcp_connect (TCP_SYN) and the LISTEN -> SYN_RECEIVED path
 * (TCP_SYN | TCP_ACK). */
static void tcp_emit_syn(struct tcb *t, unsigned char flags)
{
	unsigned char  buf[40];                  /* 20 hdr + 4 MSS + 4 NOP padding */
	struct tcp_hdr *h = (struct tcp_hdr *)buf;
	h->src_port = htons(t->local_port);
	h->dst_port = htons(t->remote_port);
	h->seq      = htonl(t->snd_nxt);
	h->ack      = htonl(t->rcv_nxt);
	h->flags    = flags;
	h->window   = htons((unsigned short)t->rcv_wnd);
	h->checksum = 0;
	h->urg      = 0;

	unsigned int off = sizeof(*h);
	buf[off++] = 2;                                  /* kind = MSS */
	buf[off++] = 4;                                  /* length */
	buf[off++] = (TCP_MSS >> 8) & 0xFF;
	buf[off++] =  TCP_MSS       & 0xFF;
	buf[off++] = 4;                                  /* kind = SACK-permitted */
	buf[off++] = 2;                                  /* length */
	while (off & 3u)
		buf[off++] = 1;                          /* NOP padding */

	h->data_off = (unsigned char)((off >> 2) << 4);
	h->checksum = tcp_checksum(t->local_ip, t->remote_ip, buf, off);
	ipv4_send(t->remote_ip, IP_PROTO_TCP, buf, off);
}

/* Emit an ACK segment that optionally carries SACK blocks describing the
 * out-of-order ranges we already hold. Up to 2 blocks per emission (matches
 * the OOO queue cap). Padded to a 4-byte boundary so data_off stays valid.
 * Plain ACKs without OOO data fall back to the no-options fast path -- this
 * keeps the wire footprint identical for the common case. */
static void tcp_emit_sack_ack(struct tcb *t)
{
	unsigned int blocks = 0;
	unsigned int starts[2], ends[2];
	for (int k = 0; k < 2; k++) {
		if (!t->ooo[k].used) continue;
		if (blocks >= 2) break;
		starts[blocks] = t->ooo[k].seq;
		ends[blocks]   = t->ooo[k].seq + t->ooo[k].len;
		blocks++;
	}
	if (blocks == 0) {
		tcp_emit(t, TCP_ACK, (void *)0, 0);
		return;
	}

	unsigned char  buf[1500];
	struct tcp_hdr *h = (struct tcp_hdr *)buf;
	h->src_port = htons(t->local_port);
	h->dst_port = htons(t->remote_port);
	h->seq      = htonl(t->snd_nxt);
	h->ack      = htonl(t->rcv_nxt);
	h->flags    = TCP_ACK;
	h->window   = htons((unsigned short)t->rcv_wnd);
	h->checksum = 0;
	h->urg      = 0;

	unsigned int off = sizeof(*h);
	buf[off++] = 1;                                /* NOP */
	buf[off++] = 1;                                /* NOP -- align SACK to 4B */
	buf[off++] = 5;                                /* kind = SACK */
	buf[off++] = (unsigned char)(2u + 8u * blocks);
	for (unsigned int b = 0; b < blocks; b++) {
		unsigned int s = htonl(starts[b]);
		unsigned int e = htonl(ends[b]);
		const unsigned char *sp = (const unsigned char *)&s;
		const unsigned char *ep = (const unsigned char *)&e;
		for (int j = 0; j < 4; j++) buf[off++] = sp[j];
		for (int j = 0; j < 4; j++) buf[off++] = ep[j];
	}
	while (off & 3u)
		buf[off++] = 0;

	h->data_off = (unsigned char)((off >> 2) << 4);
	h->checksum = tcp_checksum(t->local_ip, t->remote_ip, buf, off);
	ipv4_send(t->remote_ip, IP_PROTO_TCP, buf, off);
}

/* Scan TCP options for kind=2 (Maximum Segment Size, RFC 793). Returns the
 * announced MSS in host order, or 0 if absent / malformed. Caller clamps. */
static unsigned short tcp_parse_mss(const unsigned char *opts,
                                    unsigned int opts_len)
{
	for (unsigned int i = 0; i < opts_len; ) {
		unsigned char kind = opts[i];
		if (kind == 0) break;
		if (kind == 1) { i++; continue; }
		if (i + 1 >= opts_len) break;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > opts_len) break;
		if (kind == 2 && optlen == 4 && i + 4 <= opts_len) {
			return (unsigned short)((opts[i + 2] << 8) | opts[i + 3]);
		}
		i += optlen;
	}
	return 0;
}

/* Scan TCP options between the fixed 20-byte header and `data_off` for the
 * window-scale (kind=3, len=3) option. Returns the shift count (0..14) the
 * peer advertised, or 0 if no scale option is present or the option set is
 * malformed. RFC 7323 caps the shift at 14; anything above is clamped. */
static unsigned char tcp_parse_wscale(const unsigned char *opts,
                                      unsigned int opts_len)
{
	for (unsigned int i = 0; i < opts_len; ) {
		unsigned char kind = opts[i];
		if (kind == 0)            /* EOL */ break;
		if (kind == 1) { i++; continue; }   /* NOP padding */
		if (i + 1 >= opts_len) break;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > opts_len) break;
		if (kind == 3 && optlen == 3) {
			unsigned char shift = opts[i + 2];
			if (shift > 14) shift = 14;
			return shift;
		}
		i += optlen;
	}
	return 0;
}

/* Scan TCP options for kind=5 (SACK, RFC 2018) and return the LOWEST left
 * edge across all reported blocks, in host order. Zero means the option is
 * absent or malformed; the caller must treat zero as "no SACK info". The
 * option payload is `n * 8` bytes of [left, right] pairs, each in network
 * order. We only need the smallest left edge -- that's the start of the
 * earliest hole the peer is asking us to fill. */
static unsigned int tcp_parse_sack_lo(const unsigned char *opts,
                                      unsigned int opts_len)
{
	unsigned int lowest = 0;
	int seen = 0;
	for (unsigned int i = 0; i < opts_len; ) {
		unsigned char kind = opts[i];
		if (kind == 0) break;
		if (kind == 1) { i++; continue; }
		if (i + 1 >= opts_len) break;
		unsigned char optlen = opts[i + 1];
		if (optlen < 2 || i + optlen > opts_len) break;
		if (kind == 5 && optlen >= 10 && ((optlen - 2u) % 8u) == 0) {
			unsigned int n = (optlen - 2u) / 8u;
			for (unsigned int b = 0; b < n; b++) {
				unsigned int off = i + 2u + b * 8u;
				unsigned int left =
				    ((unsigned int)opts[off + 0] << 24) |
				    ((unsigned int)opts[off + 1] << 16) |
				    ((unsigned int)opts[off + 2] <<  8) |
				    ((unsigned int)opts[off + 3]);
				if (!seen || left < lowest) {
					lowest = left;
					seen = 1;
				}
			}
		}
		i += optlen;
	}
	return seen ? lowest : 0u;
}

/* Scale the peer's raw 16-bit window value by their advertised shift. */
static unsigned int tcp_apply_wscale(const struct tcb *t, const struct tcp_hdr *h)
{
	return ((unsigned int)ntohs(h->window)) << t->peer_wscale;
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

	/* Any incoming segment counts as activity for keepalive purposes. */
	t->last_activity_ticks  = pit_ticks();
	t->last_keepalive_ticks = 0;

	switch (t->state) {
	case TCP_LISTEN:
		/* Only a bare SYN advances a LISTEN socket; everything else gets
		 * a RST so the peer learns the port is closed for non-handshake
		 * traffic. Single-connection model -- the LISTEN slot mutates
		 * into the accepted connection; a follow-up commit could maintain
		 * a backlog of pending SYNs instead. */
		if ((h->flags & TCP_SYN) && !(h->flags & TCP_ACK)) {
			t->remote_ip   = src_ip;
			t->remote_port = src_port;
			t->snd_una     = initial_seq;
			t->snd_nxt     = initial_seq;
			t->rcv_nxt     = seg_seq + 1;
			/* Pick up the peer's window-scale + MSS before applying
			 * the window so snd_wnd starts at the correct byte
			 * count and tcp_drain_tx caps chunks correctly. */
			{
				unsigned int data_off = (h->data_off >> 4) * 4u;
				if (data_off > sizeof(struct tcp_hdr) &&
				    data_off <= len) {
					const unsigned char *opts =
					    (const unsigned char *)payload +
					        sizeof(struct tcp_hdr);
					unsigned int olen =
					    data_off - sizeof(struct tcp_hdr);
					t->peer_wscale = tcp_parse_wscale(opts, olen);
					unsigned short mss = tcp_parse_mss(opts, olen);
					if (mss >= 64 && mss <= TCP_MSS)
						t->peer_mss = mss;
				}
			}
			t->snd_wnd     = tcp_apply_wscale(t, h);
			initial_seq   += 64000u;
			t->state       = TCP_SYN_RECEIVED;
			tcp_emit_syn(t, TCP_SYN | TCP_ACK);
			t->snd_nxt += 1;            /* our SYN consumes one slot */
			tcp_arm_rto(t);
		} else if (!(h->flags & TCP_RST)) {
			tcp_emit_rst(src_ip, h);
		}
		break;

	case TCP_SYN_RECEIVED:
		if ((h->flags & TCP_ACK) && seg_ack == t->snd_nxt) {
			t->snd_una        = seg_ack;
			t->snd_wnd        = tcp_apply_wscale(t, h);
			t->state          = TCP_ESTABLISHED;
			t->last_seg_ticks = 0;      /* our SYN-ACK has been acked */
			t->retries        = 0;
		}
		break;

	case TCP_SYN_SENT:
		if ((h->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) &&
		    seg_ack == t->snd_nxt) {
			t->rcv_nxt = seg_seq + 1;
			t->snd_una = seg_ack;
			/* SYN-ACK is the only chance to learn peer scale + MSS. */
			{
				unsigned int data_off = (h->data_off >> 4) * 4u;
				if (data_off > sizeof(struct tcp_hdr) &&
				    data_off <= len) {
					const unsigned char *opts =
					    (const unsigned char *)payload +
					        sizeof(struct tcp_hdr);
					unsigned int olen =
					    data_off - sizeof(struct tcp_hdr);
					t->peer_wscale = tcp_parse_wscale(opts, olen);
					unsigned short mss = tcp_parse_mss(opts, olen);
					if (mss >= 64 && mss <= TCP_MSS)
						t->peer_mss = mss;
				}
			}
			t->snd_wnd = tcp_apply_wscale(t, h);
			t->state   = TCP_ESTABLISHED;
			t->last_seg_ticks = 0;      /* our SYN has been acked */
			t->retries        = 0;
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
			unsigned int data_off_dbg = (h->data_off >> 4) * 4u;
			unsigned int payload_dbg = (data_off_dbg <= len)
			    ? (len - data_off_dbg) : 0;
			/* Refresh the peer's SACK scoreboard. Anything <= snd_una
			 * is irrelevant (peer ACKed it cumulatively); store only
			 * the lowest left edge that lies ahead of snd_una. */
			{
				const unsigned char *opts =
				    (const unsigned char *)payload + sizeof(struct tcp_hdr);
				unsigned int olen = (data_off_dbg >= sizeof(struct tcp_hdr))
				    ? (data_off_dbg - sizeof(struct tcp_hdr)) : 0;
				unsigned int lo = tcp_parse_sack_lo(opts, olen);
				if (lo != 0 && (int)(lo - t->snd_una) > 0)
					t->peer_sack_lo = lo;
				else
					t->peer_sack_lo = 0;
			}
			/* Dup-ACK detector: ACK that does NOT advance snd_una,
			 * carries no payload, and still has unacked data in
			 * flight. Three such ACKs in a row trigger fast
			 * retransmit; anything else resets the counter. */
			if (acked == 0 && payload_dbg == 0 && inflight > 0) {
				if (t->dup_acks < 255)
					t->dup_acks++;
				if (t->dup_acks == 3) {
					t->ssthresh = inflight / 2;
					if (t->ssthresh < 2u * TCP_MSS)
						t->ssthresh = 2u * TCP_MSS;
					t->cwnd = t->ssthresh;
					tcp_retransmit(t);
				}
			} else if (acked > 0) {
				t->dup_acks = 0;
			}
			if (acked > 0 && acked <= inflight) {
				t->snd_una = seg_ack;
				/* Trim acked bytes off the head of tx_buf. */
				t->tx_head = (t->tx_head + acked) % TCP_BUFFER_SIZE;
				t->snd_wnd = tcp_apply_wscale(t, h);

				/* Slow start vs. congestion avoidance. cwnd<ssthresh:
				 * grow by `acked` (effectively doubles per RTT once the
				 * burst rolls). cwnd>=ssthresh: additive increase, grow
				 * by ~MSS per RTT independent of segment size. Cap cwnd
				 * at the tx ring -- there is no point holding a credit
				 * larger than our outstanding-data ceiling. */
				if (t->cwnd < t->ssthresh) {
					t->cwnd += acked;
				} else if (t->cwnd > 0) {
					unsigned int add = (TCP_MSS * acked) / t->cwnd;
					if (add == 0) add = 1;
					t->cwnd += add;
				}
				if (t->cwnd > TCP_BUFFER_SIZE)
					t->cwnd = TCP_BUFFER_SIZE;

				/* Fresh ACK clears the retransmit timer. If still some
				 * bytes in flight, tcp_drain_tx re-arms it; if fully
				 * caught up, leave last_seg_ticks=0 to disable RTO. */
				if (t->snd_una == t->snd_nxt) {
					t->last_seg_ticks = 0;
					t->retries        = 0;
				}
				tcp_drain_tx(t);
			}
		}

		/* Take any payload. In-order segments land in rx_buf directly
		 * and pull any queued OOO segments behind them; future-seq
		 * segments park in the OOO queue until the gap is filled. */
		unsigned int data_off = (h->data_off >> 4) * 4u;
		if (data_off >= len)
			data_off = sizeof(struct tcp_hdr);
		unsigned int data_len = len - data_off;
		if (data_len > 0) {
			const unsigned char *data =
			    (const unsigned char *)payload + data_off;
			if (seg_seq == t->rcv_nxt) {
				tcb_rx_inline(t, data, data_len);
				tcb_ooo_drain(t);
				/* Some OOO slot may still hold data ahead of the
				 * new rcv_nxt; advertise it via SACK so the peer
				 * can skip retransmitting bytes we already have. */
				tcp_emit_sack_ack(t);
			} else if ((int)(seg_seq - t->rcv_nxt) > 0) {
				/* Future seq: queue, then dup-ACK the still-
				 * expected rcv_nxt with SACK blocks describing
				 * every OOO range we hold. */
				tcb_ooo_park(t, seg_seq, data, data_len);
				tcp_emit_sack_ack(t);
			}
			/* past-seq: silently drop (already have it). */
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

int tcp_get_info(int idx, struct tcp_info *out)
{
	if (!out || idx < 0 || idx >= TCP_MAX_CONNS)
		return -1;
	struct tcb *t = &conns[idx];
	out->in_use      = (unsigned int)t->in_use;
	out->state       = (unsigned int)t->state;
	out->local_ip    = t->local_ip;
	out->remote_ip   = t->remote_ip;
	out->local_port  = t->local_port;
	out->remote_port = t->remote_port;
	out->rx_pending  = (t->rx_tail + TCP_BUFFER_SIZE - t->rx_head) %
	                   TCP_BUFFER_SIZE;
	out->tx_pending  = tx_pending(t) + (t->snd_nxt - t->snd_una);
	return 0;
}

/* Send a keepalive probe: ACK with seq = snd_una - 1. The peer must respond
 * with an ACK confirming our connection is still alive. Doesn't consume
 * sequence space, doesn't disturb the data flow. */
static void tcp_send_keepalive(struct tcb *t)
{
	unsigned int saved = t->snd_nxt;
	t->snd_nxt = t->snd_una - 1u;
	tcp_emit(t, TCP_ACK, (void *)0, 0);
	t->snd_nxt = saved;
}

void tcp_tick(void)
{
	unsigned int now = pit_ticks();

	for (int i = 0; i < TCP_MAX_CONNS; i++) {
		struct tcb *t = &conns[i];
		if (!t->in_use)
			continue;

		/* Keepalive path: idle ESTABLISHED connections get a periodic
		 * probe so dead peers / NAT rebinds surface as a CLOSED state
		 * instead of hanging forever in tcp_recv. Runs independently of
		 * the retransmit loop below. */
		if (t->state == TCP_ESTABLISHED && t->last_activity_ticks != 0) {
			unsigned int idle = now - t->last_activity_ticks;
			if (idle >= TCP_KEEPALIVE_IDLE) {
				unsigned int since_probe = (t->last_keepalive_ticks == 0)
				    ? TCP_KEEPALIVE_INTERVAL
				    : (now - t->last_keepalive_ticks);
				if (since_probe >= TCP_KEEPALIVE_INTERVAL) {
					tcp_send_keepalive(t);
					t->last_keepalive_ticks = now;
				}
			}
		}

		if (t->last_seg_ticks == 0)
			continue;
		if (t->snd_una == t->snd_nxt) {
			/* Nothing unacked anymore; defensive clear in case the ACK
			 * path missed it. */
			t->last_seg_ticks = 0;
			continue;
		}
		if ((now - t->last_seg_ticks) < t->rto_ticks)
			continue;

		if (t->retries >= TCP_MAX_RETRIES) {
			/* Too many tries -- drop the connection. The peer might be
			 * gone or unreachable; the user space will see CLOSED on
			 * tcp_get_state and bail out. */
			t->state  = TCP_CLOSED;
			t->in_use = 0;
			continue;
		}

		/* Loss collapse before retransmitting: halve ssthresh, drop
		 * cwnd to one segment so we re-probe the path with slow start. */
		unsigned int half = (t->snd_nxt - t->snd_una) / 2u;
		if (half < 2u * TCP_MSS)
			half = 2u * TCP_MSS;
		t->ssthresh = half;
		t->cwnd     = TCP_MSS;

		tcp_retransmit(t);
		t->retries++;
		t->rto_ticks *= 2;                  /* exponential backoff */
		if (t->rto_ticks > TCP_RTO_MAX)
			t->rto_ticks = TCP_RTO_MAX;
		t->last_seg_ticks = now;
	}
}
