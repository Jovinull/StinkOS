/* ICMP echo handling. The reply path mirrors the request payload verbatim
 * (same id, same sequence, same data) -- which is what every standard ping
 * implementation expects. The send-echo-request helper exists mostly so an
 * eventual `ping` user app can drive it via a syscall later. */
#include "icmp.h"
#include "ipv4.h"
#include "ethernet.h"

static unsigned char scratch[ETH_MAX_PAYLOAD - 20];     /* IP header eats 20 */

int icmp_send_echo_request(ipv4_t dst, unsigned short identifier,
                           unsigned short sequence,
                           const void *payload, unsigned int payload_len)
{
	if (payload_len > sizeof(scratch) - sizeof(struct icmp_hdr))
		return -1;

	struct icmp_hdr *h = (struct icmp_hdr *)scratch;
	h->type       = ICMP_TYPE_ECHO_REQUEST;
	h->code       = 0;
	h->checksum   = 0;
	h->identifier = htons(identifier);
	h->sequence   = htons(sequence);

	const unsigned char *src = (const unsigned char *)payload;
	for (unsigned int i = 0; i < payload_len; i++)
		scratch[sizeof(*h) + i] = src[i];

	unsigned int total = sizeof(*h) + payload_len;
	h->checksum = ipv4_checksum(scratch, total);

	return ipv4_send(dst, IP_PROTO_ICMP, scratch, total);
}

void icmp_handle(const void *payload, unsigned int len, ipv4_t src_ip)
{
	if (len < sizeof(struct icmp_hdr))
		return;

	const struct icmp_hdr *h = (const struct icmp_hdr *)payload;

	if (h->type != ICMP_TYPE_ECHO_REQUEST)
		return;

	/* Mirror the entire packet back with type swapped to echo reply.
	 * Recomputing the checksum incrementally is cute but the full
	 * recompute is trivial at our scale. */
	if (len > sizeof(scratch))
		return;
	for (unsigned int i = 0; i < len; i++)
		scratch[i] = ((const unsigned char *)payload)[i];

	struct icmp_hdr *reply = (struct icmp_hdr *)scratch;
	reply->type     = ICMP_TYPE_ECHO_REPLY;
	reply->checksum = 0;
	reply->checksum = ipv4_checksum(scratch, len);

	ipv4_send(src_ip, IP_PROTO_ICMP, scratch, len);
}
