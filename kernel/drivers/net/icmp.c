/* ICMP echo handling. The reply path mirrors the request payload verbatim
 * (same id, same sequence, same data) -- which is what every standard ping
 * implementation expects. The send-echo-request helper exists mostly so an
 * eventual `ping` user app can drive it via a syscall later. */
#include "icmp.h"
#include "ipv4.h"
#include "ethernet.h"

static unsigned char scratch[ETH_MAX_PAYLOAD - 20];     /* IP header eats 20 */

/* Outstanding outbound-ping state (see icmp_ping_arm). */
static volatile int   ping_pending;
static unsigned short ping_id;
static unsigned short ping_seq;
static volatile int   ping_replied;

void icmp_ping_arm(unsigned short identifier, unsigned short sequence)
{
	ping_id      = identifier;
	ping_seq     = sequence;
	ping_replied = 0;
	ping_pending = 1;
}

int icmp_ping_replied(void) { return ping_replied; }

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

	if (h->type == ICMP_TYPE_ECHO_REPLY) {     /* reply to our own ping */
		if (ping_pending &&
		    ntohs(h->identifier) == ping_id &&
		    ntohs(h->sequence)   == ping_seq) {
			ping_replied = 1;
			ping_pending = 0;
		}
		return;
	}

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

/* RFC 792 "Destination Unreachable": 8-byte header (type, code, checksum,
 * unused 32-bit) + the original IP header + first 8 bytes of its payload.
 * Most TCP/UDP stacks use those bytes to route the rejection back to the
 * right socket. */
int icmp_send_unreachable(ipv4_t dst, unsigned char code,
                          const void *orig_ip_packet, unsigned int orig_len)
{
	unsigned int echo = orig_len;
	if (echo > 28u) echo = 28u;            /* 20-byte IP hdr + 8 payload bytes */
	unsigned int total = 8u + echo;
	if (total > sizeof(scratch))
		return -1;

	scratch[0] = ICMP_TYPE_DEST_UNREACH;
	scratch[1] = code;
	scratch[2] = 0; scratch[3] = 0;        /* checksum slot */
	scratch[4] = 0; scratch[5] = 0;        /* unused */
	scratch[6] = 0; scratch[7] = 0;
	const unsigned char *o = (const unsigned char *)orig_ip_packet;
	for (unsigned int i = 0; i < echo; i++)
		scratch[8 + i] = o[i];

	unsigned short ck = ipv4_checksum(scratch, total);
	scratch[2] = (unsigned char)(ck & 0xFF);
	scratch[3] = (unsigned char)((ck >> 8) & 0xFF);
	return ipv4_send(dst, IP_PROTO_ICMP, scratch, total);
}
