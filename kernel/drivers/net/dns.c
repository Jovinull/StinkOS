/* Minimal single-request DNS A-record resolver.
 *
 * Send: build a 12-byte DNS header (transaction id, QR=0, recursion-desired),
 *       encode the question name as length-prefixed labels (e.g. "example.com"
 *       -> "\x07example\x03com\x00"), append QTYPE=A and QCLASS=IN.
 * Recv: walk the answer section looking for the first A record; cache the IP.
 *
 * We use a fixed UDP source port (32768) and a single transaction at a time
 * -- enough for a serial "resolve then connect" client flow. */
#include "dns.h"
#include "udp.h"
#include "dhcp.h"
#include "interrupts.h"           /* pit_ticks for cache TTL */

#define DNS_PORT          53
#define LOCAL_PORT        32768
#define QTYPE_A           1
#define QCLASS_IN         1
#define DNS_FLAG_RD       0x0100u   /* recursion desired */
#define DNS_FLAG_QR_REPLY 0x8000u

struct dns_hdr {
	unsigned short id;
	unsigned short flags;
	unsigned short qd_count;
	unsigned short an_count;
	unsigned short ns_count;
	unsigned short ar_count;
} __attribute__((packed));

static unsigned short    request_id     = 1;
static unsigned short    current_id;
static int               port_bound;
static int               answer_ready;
static ipv4_t            answer_ip;

/* Retransmit state for the in-flight query. last_send_tick is set to
 * pit_ticks() on every send (initial + retry); send_retries counts how
 * many copies we've already sent. A 2-second timeout per attempt and 3
 * retries total puts the worst-case latency at ~8 seconds before the
 * caller's app-level timeout kicks in. */
#define DNS_RETRY_TICKS   200u    /* 2 s at 100 Hz PIT */
#define DNS_MAX_RETRIES   3
static unsigned int       last_send_tick;
static unsigned char      send_retries;

/* Response cache. 8 entries, fixed 60-second TTL regardless of the per-
 * record TTL the server actually sent -- a hobby OS rarely cares about
 * sub-minute freshness, and dropping the TTL parser keeps this small. */
#define DNS_CACHE_SIZE   8
#define DNS_CACHE_TTL    6000u           /* 60 s at 100 Hz PIT */
#define DNS_CACHE_NAME   64

struct dns_cache_entry {
	int          in_use;
	char         name[DNS_CACHE_NAME];
	ipv4_t       ip;
	unsigned int filled_at;
};

static struct dns_cache_entry dns_cache[DNS_CACHE_SIZE];
static int dns_cache_round;                     /* round-robin replace cursor */

static int names_eq(const char *a, const char *b)
{
	int i = 0;
	while (a[i] && b[i] && a[i] == b[i]) i++;
	return a[i] == '\0' && b[i] == '\0';
}

static struct dns_cache_entry *dns_cache_find(const char *name)
{
	unsigned int now = pit_ticks();
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		struct dns_cache_entry *e = &dns_cache[i];
		if (!e->in_use)
			continue;
		if ((unsigned int)(now - e->filled_at) > DNS_CACHE_TTL) {
			e->in_use = 0;
			continue;
		}
		if (names_eq(e->name, name))
			return e;
	}
	return 0;
}

static void dns_cache_put(const char *name, ipv4_t ip)
{
	if (!name || !name[0] || ip == 0)
		return;
	/* Reuse existing slot for the same name, else round-robin replace. */
	for (int i = 0; i < DNS_CACHE_SIZE; i++) {
		if (dns_cache[i].in_use && names_eq(dns_cache[i].name, name)) {
			dns_cache[i].ip        = ip;
			dns_cache[i].filled_at = pit_ticks();
			return;
		}
	}
	struct dns_cache_entry *e = &dns_cache[dns_cache_round];
	dns_cache_round = (dns_cache_round + 1) % DNS_CACHE_SIZE;
	int k = 0;
	while (k < DNS_CACHE_NAME - 1 && name[k]) {
		e->name[k] = name[k];
		k++;
	}
	e->name[k]  = '\0';
	e->ip        = ip;
	e->filled_at = pit_ticks();
	e->in_use    = 1;
}

/* Last queried name, captured so the reply path can write into the cache
 * (the wire reply itself doesn't carry the bare name back -- it would
 * need to be reconstructed from the question section). */
static char last_query_name[DNS_CACHE_NAME];

/* Encode "example.com" -> "\x07example\x03com\x00" into out. Returns the
 * number of bytes written, or -1 if buffer would overflow. */
static int encode_qname(const char *name, unsigned char *out, unsigned int cap)
{
	unsigned int written = 0;
	while (*name) {
		const char *seg = name;
		unsigned int seg_len = 0;
		while (name[seg_len] && name[seg_len] != '.')
			seg_len++;
		if (seg_len == 0 || seg_len > 63 || written + seg_len + 1 >= cap)
			return -1;
		out[written++] = (unsigned char)seg_len;
		for (unsigned int i = 0; i < seg_len; i++)
			out[written++] = (unsigned char)seg[i];
		name += seg_len;
		if (*name == '.')
			name++;
	}
	if (written + 1 >= cap)
		return -1;
	out[written++] = 0;                      /* root label */
	return (int)written;
}

/* Skip a DNS-encoded name in the answer section. Handles label compression
 * (0xC0XX pointers). Returns the number of bytes consumed from 'p' (a pointer
 * jump always consumes 2 bytes). */
static unsigned int skip_name(const unsigned char *base,
                              const unsigned char *p,
                              unsigned int remaining)
{
	(void)base;
	unsigned int consumed = 0;
	while (remaining > 0) {
		unsigned char b = *p;
		if ((b & 0xC0u) == 0xC0u)
			return consumed + 2;
		if (b == 0)
			return consumed + 1;
		if (b + 1u > remaining)
			return remaining;
		consumed += b + 1u;
		p        += b + 1u;
		remaining -= b + 1u;
	}
	return consumed;
}

static void on_packet(ipv4_t src_ip, unsigned short src_port,
                      const void *payload, unsigned int len)
{
	(void)src_ip; (void)src_port;
	if (len < sizeof(struct dns_hdr))
		return;

	const struct dns_hdr *h = (const struct dns_hdr *)payload;
	if (ntohs(h->id) != current_id)
		return;
	if (!(ntohs(h->flags) & DNS_FLAG_QR_REPLY))
		return;

	unsigned int an_count = ntohs(h->an_count);
	if (an_count == 0)
		return;

	const unsigned char *p   = (const unsigned char *)payload + sizeof(*h);
	const unsigned char *end = (const unsigned char *)payload + len;

	/* Skip the question section: one qname + QTYPE(2) + QCLASS(2). */
	unsigned int qd_count = ntohs(h->qd_count);
	for (unsigned int i = 0; i < qd_count && p < end; i++) {
		p += skip_name(payload, p, (unsigned int)(end - p));
		if (p + 4 > end) return;
		p += 4;
	}

	/* Walk answers; take the first A record we see. */
	for (unsigned int i = 0; i < an_count && p < end; i++) {
		p += skip_name(payload, p, (unsigned int)(end - p));
		if (p + 10 > end) return;
		unsigned short type   = (unsigned short)((p[0] << 8) | p[1]);
		unsigned short rdlen  = (unsigned short)((p[8] << 8) | p[9]);
		p += 10;
		if (p + rdlen > end) return;
		if (type == QTYPE_A && rdlen == 4) {
			unsigned char *ob = (unsigned char *)&answer_ip;
			ob[0] = p[0]; ob[1] = p[1]; ob[2] = p[2]; ob[3] = p[3];
			answer_ready = 1;
			last_send_tick = 0;        /* stop retransmit pump */
			dns_cache_put(last_query_name, answer_ip);
			return;
		}
		p += rdlen;
	}
}

int dns_resolve(const char *name)
{
	if (!name)
		return -1;

	/* Cache hit: skip the wire entirely. answer_ready flips immediately
	 * so the caller's `dns_ready()` poll succeeds on the first tick. */
	struct dns_cache_entry *hit = dns_cache_find(name);
	if (hit) {
		answer_ip    = hit->ip;
		answer_ready = 1;
		return 0;
	}

	if (!dhcp_bound() || dhcp_get_dns() == 0)
		return -1;

	if (!port_bound) {
		if (udp_bind(LOCAL_PORT, on_packet) != 0)
			return -1;
		port_bound = 1;
	}

	/* Save name so on_packet can fill the cache when the reply arrives. */
	int k = 0;
	while (k < DNS_CACHE_NAME - 1 && name[k]) {
		last_query_name[k] = name[k];
		k++;
	}
	last_query_name[k] = '\0';

	answer_ready = 0;
	answer_ip    = 0;
	current_id   = request_id++;

	unsigned char pkt[512];
	struct dns_hdr *h = (struct dns_hdr *)pkt;
	h->id       = htons(current_id);
	h->flags    = htons(DNS_FLAG_RD);
	h->qd_count = htons(1);
	h->an_count = 0;
	h->ns_count = 0;
	h->ar_count = 0;

	int qn = encode_qname(name, pkt + sizeof(*h),
	                      sizeof(pkt) - sizeof(*h) - 4);
	if (qn < 0)
		return -1;

	unsigned int off = sizeof(*h) + (unsigned int)qn;
	pkt[off++] = 0; pkt[off++] = QTYPE_A;
	pkt[off++] = 0; pkt[off++] = QCLASS_IN;

	last_send_tick = pit_ticks();
	send_retries   = 0;
	return udp_send(dhcp_get_dns(), DNS_PORT, LOCAL_PORT, pkt, off);
}

/* Re-encode + re-send the same query packet (same id, same qname). The
 * cached `last_query_name` lets us rebuild the wire format without
 * holding a static byte buffer. */
static void dns_retransmit(void)
{
	if (!dhcp_bound() || dhcp_get_dns() == 0)
		return;
	unsigned char pkt[512];
	struct dns_hdr *h = (struct dns_hdr *)pkt;
	h->id       = htons(current_id);
	h->flags    = htons(DNS_FLAG_RD);
	h->qd_count = htons(1);
	h->an_count = 0;
	h->ns_count = 0;
	h->ar_count = 0;
	int qn = encode_qname(last_query_name, pkt + sizeof(*h),
	                      sizeof(pkt) - sizeof(*h) - 4);
	if (qn < 0)
		return;
	unsigned int off = sizeof(*h) + (unsigned int)qn;
	pkt[off++] = 0; pkt[off++] = QTYPE_A;
	pkt[off++] = 0; pkt[off++] = QCLASS_IN;
	udp_send(dhcp_get_dns(), DNS_PORT, LOCAL_PORT, pkt, off);
}

void dns_tick(void)
{
	if (answer_ready || last_send_tick == 0)
		return;                    /* nothing in flight */
	if (send_retries >= DNS_MAX_RETRIES) {
		/* Exhausted -- mark the slot inert so we don't retry forever.
		 * The caller's app-level timeout decides what to do next. */
		last_send_tick = 0;
		return;
	}
	unsigned int now = pit_ticks();
	if ((now - last_send_tick) < DNS_RETRY_TICKS)
		return;
	dns_retransmit();
	last_send_tick = now;
	send_retries++;
}

int    dns_ready(void)   { return answer_ready; }
ipv4_t dns_get_ip(void)  { return answer_ip; }
