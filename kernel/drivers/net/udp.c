/* UDP send + dispatch. Outgoing packets get a header and an optional UDP
 * checksum (we leave it 0, which is legal on IPv4 and saves a pseudo-header
 * pass). Incoming packets dispatch by destination port through a small
 * fixed-size handler table -- enough for DHCP, DNS, and a couple of apps. */
#include "udp.h"
#include "ipv4.h"
#include "ethernet.h"
#include "icmp.h"           /* send port-unreachable for unbound dst ports */

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

void udp_handle(const void *payload, unsigned int len, ipv4_t src_ip)
{
	if (len < sizeof(struct udp_hdr))
		return;

	const struct udp_hdr *h = (const struct udp_hdr *)payload;
	unsigned short dst_port = ntohs(h->dst_port);
	unsigned short total    = ntohs(h->length);
	if (total > len || total < sizeof(*h))
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

	icmp_send_unreachable(src_ip, ICMP_CODE_PORT_UNREACH, embed, 20u + copy);
}
