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
			return;
		}
		p += rdlen;
	}
}

int dns_resolve(const char *name)
{
	if (!dhcp_bound() || dhcp_get_dns() == 0)
		return -1;

	if (!port_bound) {
		if (udp_bind(LOCAL_PORT, on_packet) != 0)
			return -1;
		port_bound = 1;
	}

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

	return udp_send(dhcp_get_dns(), DNS_PORT, LOCAL_PORT, pkt, off);
}

int    dns_ready(void)   { return answer_ready; }
ipv4_t dns_get_ip(void)  { return answer_ip; }
