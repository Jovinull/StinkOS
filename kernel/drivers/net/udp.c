/* UDP send + dispatch. Outgoing packets get a header and an optional UDP
 * checksum (we leave it 0, which is legal on IPv4 and saves a pseudo-header
 * pass). Incoming packets dispatch by destination port through a small
 * fixed-size handler table -- enough for DHCP, DNS, and a couple of apps. */
#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "icmp.h"           /* send port-unreachable for unbound dst ports */
#include "interrupts.h"     /* pit_ticks() for ICMP unreachable rate limit */

#define UDP_MAX_HANDLERS 8

struct udp_binding {
	unsigned short port;            /* host order */
	udp_handler_t  cb;
	int            in_use;
};

static struct udp_binding bindings[UDP_MAX_HANDLERS];
static unsigned char       scratch[ETH_MAX_PAYLOAD - 20];  /* less IP header */

int udp_send(ipv4_t dst_ip, unsigned short dst_port,
             unsigned short src_port,
             const void *payload, unsigned int len)
{
	if (len > sizeof(scratch) - sizeof(struct udp_hdr))
		return -1;

	struct udp_hdr *h = (struct udp_hdr *)scratch;
	h->src_port = htons(src_port);
	h->dst_port = htons(dst_port);
	h->length   = htons((unsigned short)(sizeof(*h) + len));
	h->checksum = 0;                /* RFC 768: 0 = "no checksum" on IPv4 */

	const unsigned char *src = (const unsigned char *)payload;
	for (unsigned int i = 0; i < len; i++)
		scratch[sizeof(*h) + i] = src[i];

	return ipv4_send(dst_ip, IP_PROTO_UDP, scratch, sizeof(*h) + len);
}

int udp_bind(unsigned short port, udp_handler_t cb)
{
	for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
		if (bindings[i].in_use && bindings[i].port == port)
			return -1;          /* already bound */
	}
	for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
		if (!bindings[i].in_use) {
			bindings[i].port   = port;
			bindings[i].cb     = cb;
			bindings[i].in_use = 1;
			return 0;
		}
	}
	return -1;
}

void udp_unbind(unsigned short port)
{
	for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
		if (bindings[i].in_use && bindings[i].port == port) {
			bindings[i].in_use = 0;
			return;
		}
	}
}

/* One's-complement verify of an incoming UDP packet against the IPv4
 * pseudo-header (RFC 768 / RFC 1071). Returns 1 if the checksum is
 * correct OR if the sender opted out by leaving it zero, 0 otherwise.
 * The packet bytes are NOT modified -- the existing checksum field is
 * included in the sum, which folds to 0xFFFF when valid. */
static int udp_checksum_ok(ipv4_t src, ipv4_t dst,
                           const unsigned char *pkt, unsigned int total)
{
	if (((unsigned short)pkt[6] << 8 | pkt[7]) == 0)
		return 1;          /* sender opted out -- legal on IPv4 */

	unsigned int sum = 0;
	const unsigned char *p;

	/* Pseudo-header: src_ip, dst_ip, zero, protocol(17=UDP), udp_length. */
	unsigned char ph[12];
	ph[0] = (unsigned char)(src      & 0xFF);
	ph[1] = (unsigned char)((src >>  8) & 0xFF);
	ph[2] = (unsigned char)((src >> 16) & 0xFF);
	ph[3] = (unsigned char)((src >> 24) & 0xFF);
	ph[4] = (unsigned char)(dst      & 0xFF);
	ph[5] = (unsigned char)((dst >>  8) & 0xFF);
	ph[6] = (unsigned char)((dst >> 16) & 0xFF);
	ph[7] = (unsigned char)((dst >> 24) & 0xFF);
	ph[8] = 0;
	ph[9] = 17;
	ph[10] = (unsigned char)((total >> 8) & 0xFF);
	ph[11] = (unsigned char)( total       & 0xFF);

	p = ph;
	for (unsigned int i = 0; i + 1 < sizeof(ph); i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];

	p = pkt;
	for (unsigned int i = 0; i + 1 < total; i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];
	if (total & 1)
		sum += (unsigned int)p[total - 1] << 8;

	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return (sum & 0xFFFFu) == 0xFFFFu;
}

void udp_handle(const void *payload, unsigned int len, ipv4_t src_ip)
{
	if (len < sizeof(struct udp_hdr))
		return;

	const struct udp_hdr *h = (const struct udp_hdr *)payload;
	unsigned short dst_port = ntohs(h->dst_port);
	unsigned short total    = ntohs(h->length);
	if (total > len || total < sizeof(*h))
		return;

	/* RFC 1122 §4.1.3.5: drop datagrams whose source claims to be
	 * broadcast / multicast / the unspecified address. The "no handler"
	 * path below would otherwise bounce an ICMP unreachable straight to
	 * the LAN broadcast group (the rate limiter caps the splash but a
	 * single packet per ~330ms is still wasted bandwidth and a tiny
	 * reflector vector). DHCP DISCOVER is an exception we accept on
	 * port 68 before this point lands -- but DHCPDISCOVER comes from
	 * 0.0.0.0 ("we don't have an IP yet"), and our DHCP client BINDS
	 * port 68 so it skips the no-handler path anyway. */
	if (src_ip != 0 && (src_ip == 0xFFFFFFFFu ||
	                    (src_ip & 0x000000F0u) == 0x000000E0u))
		return;

	/* Validate the UDP checksum BEFORE handing the payload to any
	 * upper-layer parser. A non-zero checksum that fails verification
	 * means the packet was corrupted in flight; silently dropping is
	 * safer than feeding garbage to DHCP / DNS state machines. */
	if (!udp_checksum_ok(src_ip, net_get_local_ip(),
	                     (const unsigned char *)payload, total))
		return;

	for (int i = 0; i < UDP_MAX_HANDLERS; i++) {
		if (bindings[i].in_use && bindings[i].port == dst_port) {
			const unsigned char *body = (const unsigned char *)payload + sizeof(*h);
			bindings[i].cb(src_ip, ntohs(h->src_port),
			               body, total - sizeof(*h));
			return;
		}
	}

	/* No handler matched. Synthesise the minimum IP header + first 8 UDP
	 * bytes that RFC 792 wants embedded in the ICMP body, then bounce a
	 * port-unreachable so the peer learns to stop sending. */
	unsigned char embed[28];
	embed[0] = 0x45;                      /* IPv4, IHL=5 */
	embed[1] = 0;                         /* TOS */
	unsigned short tl = (unsigned short)(20u + (total < 8u ? total : 8u));
	embed[2] = (unsigned char)((tl >> 8) & 0xFF);
	embed[3] = (unsigned char)(tl & 0xFF);
	embed[4] = 0; embed[5] = 0;           /* id */
	embed[6] = 0; embed[7] = 0;           /* flags + fragoff */
	embed[8] = IPV4_DEFAULT_TTL;
	embed[9] = IP_PROTO_UDP;
	embed[10] = 0; embed[11] = 0;         /* checksum -- peers do not verify */
	ipv4_t local = net_get_local_ip();
	embed[12] = (unsigned char)(src_ip      & 0xFF);
	embed[13] = (unsigned char)((src_ip >>  8) & 0xFF);
	embed[14] = (unsigned char)((src_ip >> 16) & 0xFF);
	embed[15] = (unsigned char)((src_ip >> 24) & 0xFF);
	embed[16] = (unsigned char)(local      & 0xFF);
	embed[17] = (unsigned char)((local >>  8) & 0xFF);
	embed[18] = (unsigned char)((local >> 16) & 0xFF);
	embed[19] = (unsigned char)((local >> 24) & 0xFF);
	unsigned int copy = total < 8u ? total : 8u;
	const unsigned char *p = (const unsigned char *)payload;
	for (unsigned int i = 0; i < copy; i++)
		embed[20 + i] = p[i];

	/* RFC 1812 §4.3.2.8: throttle ICMP "destination unreachable" replies
	 * so a flood of packets to closed ports cannot turn the host into an
	 * amplifier. PIT is 100 Hz, so a 33-tick minimum interval caps us at
	 * ~3 unreachable replies per second, plenty for a real misconfigured
	 * peer but useless to an attacker. State is global rather than per-
	 * peer; per-peer accounting would need a table we have no room for. */
	{
		static unsigned int last_unreach_tick;
		unsigned int now = pit_ticks();
		if (last_unreach_tick != 0 && (now - last_unreach_tick) < 33u)
			return;
		last_unreach_tick = now;
	}

	icmp_send_unreachable(src_ip, ICMP_CODE_PORT_UNREACH, embed, 20u + copy);
}
