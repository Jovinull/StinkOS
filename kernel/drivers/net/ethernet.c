/* Ethernet II frame send + dispatch. The 14-byte header is built directly in
 * a stack buffer; payload is appended; the whole thing goes to e1000_send_frame
 * in one shot. Receive dispatch hands the payload (header-stripped) to the
 * registered upper layer based on the ethertype field. */
#include "ethernet.h"
#include "e1000.h"
#include "serial.h"

/* Forward decls for the upper layers we dispatch to. Defined in the per-
 * protocol files. */
void arp_handle(const void *payload, unsigned int len);
void ip_handle(const void *payload, unsigned int len);

int eth_send(const unsigned char dst_mac[ETH_ADDR_LEN],
             unsigned short ethertype,
             const void *payload, unsigned int payload_len)
{
	if (!dst_mac || (payload_len > 0 && !payload) || payload_len > ETH_MAX_PAYLOAD)
		return -1;

	unsigned char  frame[ETH_MAX_FRAME];
	struct eth_hdr h;

	for (int i = 0; i < ETH_ADDR_LEN; i++)
		h.dst[i] = dst_mac[i];
	net_get_local_mac(h.src);
	h.ethertype = htons(ethertype);

	const unsigned char *hb  = (const unsigned char *)&h;
	const unsigned char *pld = (const unsigned char *)payload;

	for (unsigned int i = 0; i < ETH_HDR_LEN; i++)
		frame[i] = hb[i];
	for (unsigned int i = 0; i < payload_len; i++)
		frame[ETH_HDR_LEN + i] = pld[i];

	return e1000_send_frame(frame, ETH_HDR_LEN + payload_len);
}

void eth_handle_frame(const void *frame, unsigned int len)
{
	/* Drop frames that are too short to carry a header OR longer than the
	 * standard MTU. The NIC layer already caps RX, but the upper-layer
	 * dispatch must not trust that contract -- a single oversized frame
	 * slipping through would let ARP/IPv4 parse past their own length
	 * fields. Jumbo support would relax the upper bound; we do not. */
	if (len < ETH_HDR_LEN || len > ETH_MAX_FRAME)
		return;

	const struct eth_hdr  *h   = (const struct eth_hdr *)frame;
	const unsigned char   *pld = (const unsigned char *)frame + ETH_HDR_LEN;
	unsigned int           pld_len = len - ETH_HDR_LEN;
	unsigned short         et  = ntohs(h->ethertype);

	switch (et) {
	case ETHERTYPE_ARP:
		arp_handle(pld, pld_len);
		break;
	case ETHERTYPE_IPV4:
		ip_handle(pld, pld_len);
		break;
	default:
		/* Unhandled frame type (LLC, MPLS, VLAN, etc.) -- drop silently
		 * rather than spam the log. */
		break;
	}
}
