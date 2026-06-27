/* IPv4 layer: send + dispatch. The send path builds a 20-byte header on the
 * stack, resolves the destination MAC via ARP, and ships the assembled
 * datagram down to eth_send. The receive path strips the header, validates
 * the checksum, and routes by protocol to ICMP / UDP / TCP. */
#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "dhcp.h"           /* mask + router lookup for routing decisions */

/* Forward declarations for upper-layer dispatchers. icmp / udp / tcp .c
 * files override these temporary stubs in net.c when they land. */
void icmp_handle(const void *payload, unsigned int len, ipv4_t src_ip);
void udp_handle(const void *payload, unsigned int len, ipv4_t src_ip);
void tcp_handle(const void *payload, unsigned int len, ipv4_t src_ip);

#define IP_FLAG_MF       0x2000u    /* more fragments */
#define IP_FRAGOFF_MASK  0x1FFFu

static unsigned short ip_id_counter = 1;

unsigned short ipv4_checksum(const void *data, unsigned int len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int sum = 0;

	for (unsigned int i = 0; i + 1 < len; i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];
	if (len & 1)
		sum += (unsigned int)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return htons((unsigned short)(~sum & 0xFFFFu));
}

int ipv4_send(ipv4_t dst, unsigned char protocol,
              const void *payload, unsigned int payload_len)
{
	if (payload_len > ETH_MAX_PAYLOAD - sizeof(struct ipv4_hdr))
		return -1;

	mac_t dst_mac;
	if (dst == 0xFFFFFFFFu) {
		/* 255.255.255.255 (limited broadcast): no ARP, dst MAC is the
		 * Ethernet broadcast address. DHCPDISCOVER and similar
		 * bootstrap traffic rely on this path. */
		for (int i = 0; i < 6; i++)
			dst_mac[i] = 0xFF;
	} else {
		/* Routing decision: if dst is off our subnet and DHCP gave us a
		 * default gateway, forward through that gateway. The IP header
		 * still carries the original dst_ip -- only the layer-2 ARP
		 * lookup is redirected to the router's MAC. */
		ipv4_t local  = net_get_local_ip();
		ipv4_t mask   = dhcp_get_subnet_mask();
		ipv4_t router = dhcp_get_router();
		ipv4_t arp_target = dst;
		if (mask != 0 && router != 0 &&
		    ((local & mask) != (dst & mask))) {
			arp_target = router;
		}
		if (!arp_lookup(arp_target, dst_mac)) {
			/* No L2 binding yet; kick off resolution and let the
			 * caller retry on a future tick. */
			arp_send_request(arp_target);
			return -1;
		}
	}

	unsigned char  packet[ETH_MAX_PAYLOAD];
	struct ipv4_hdr *h = (struct ipv4_hdr *)packet;

	h->ver_ihl        = 0x45;                /* IPv4, IHL = 5 dwords */
	h->tos            = 0;
	h->total_length   = htons((unsigned short)(sizeof(*h) + payload_len));
	h->id             = htons(ip_id_counter++);
	h->flags_fragoff  = 0;
	h->ttl            = IPV4_DEFAULT_TTL;
	h->protocol       = protocol;
	h->checksum       = 0;
	h->src_ip         = net_get_local_ip();
	h->dst_ip         = dst;
	h->checksum       = ipv4_checksum(h, sizeof(*h));

	const unsigned char *pld = (const unsigned char *)payload;
	for (unsigned int i = 0; i < payload_len; i++)
		packet[sizeof(*h) + i] = pld[i];

	return eth_send(dst_mac, ETHERTYPE_IPV4, packet, sizeof(*h) + payload_len);
}

void ip_handle(const void *payload, unsigned int len)
{
	if (len < sizeof(struct ipv4_hdr))
		return;

	const struct ipv4_hdr *h = (const struct ipv4_hdr *)payload;

	/* IPv4 only, no options. ihl < 5 is malformed. */
	if ((h->ver_ihl >> 4) != 4)
		return;
	unsigned int ihl = (h->ver_ihl & 0x0F) * 4;
	if (ihl < sizeof(struct ipv4_hdr) || ihl > len)
		return;

	unsigned int total = ntohs(h->total_length);
	if (total > len || total < ihl)
		return;

	/* Drop fragments (no reassembly today). */
	unsigned short ff = ntohs(h->flags_fragoff);
	if ((ff & IP_FRAGOFF_MASK) != 0 || (ff & IP_FLAG_MF))
		return;

	/* Drop if not for us (and not broadcast). */
	ipv4_t local = net_get_local_ip();
	if (h->dst_ip != local && h->dst_ip != 0xFFFFFFFFu && local != 0)
		return;

	/* Verify the header checksum. */
	if (ipv4_checksum(h, ihl) != 0)
		return;

	const unsigned char *pld = (const unsigned char *)payload + ihl;
	unsigned int         pld_len = total - ihl;

	switch (h->protocol) {
	case IP_PROTO_ICMP: icmp_handle(pld, pld_len, h->src_ip); break;
	case IP_PROTO_UDP:  udp_handle(pld, pld_len, h->src_ip);  break;
	case IP_PROTO_TCP:  tcp_handle(pld, pld_len, h->src_ip);  break;
	default: break;
	}
}
