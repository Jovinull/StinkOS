/* ICMP (RFC 792) -- the minimum needed for `ping` to work in both directions.
 * We respond to echo requests and provide a helper to send our own. */
#ifndef ICMP_H
#define ICMP_H

#include "net.h"

#define ICMP_TYPE_ECHO_REPLY     0
#define ICMP_TYPE_DEST_UNREACH   3
#define ICMP_TYPE_ECHO_REQUEST   8

/* RFC 792 codes for type=3 (Destination Unreachable). */
#define ICMP_CODE_NET_UNREACH    0
#define ICMP_CODE_HOST_UNREACH   1
#define ICMP_CODE_PROTO_UNREACH  2
#define ICMP_CODE_PORT_UNREACH   3

struct icmp_hdr {
	unsigned char  type;
	unsigned char  code;
	unsigned short checksum;
	unsigned short identifier;
	unsigned short sequence;
} __attribute__((packed));

/* Send an echo request to 'dst', with 'payload' bytes appended after the
 * 8-byte ICMP header. Returns the bytes-on-wire on success, -1 on failure. */
int  icmp_send_echo_request(ipv4_t dst, unsigned short identifier,
                            unsigned short sequence,
                            const void *payload, unsigned int payload_len);

/* Called by ipv4.c on IP_PROTO_ICMP packets. dst_ip is the destination
 * the IPv4 layer parsed before dispatch -- used by the echo-request
 * branch to drop Smurf-style broadcast pings. */
void icmp_handle(const void *payload, unsigned int len,
                 ipv4_t src_ip, ipv4_t dst_ip);

/* Echo-reply wait, for an outbound ping driven by the SYS_PING syscall. Arm
 * with the id/sequence about to be sent, then poll icmp_ping_replied() while
 * pumping the receive path; icmp_handle flags it when the matching reply lands.
 * Single outstanding ping at a time (the stack is single-threaded). */
void icmp_ping_arm(unsigned short identifier, unsigned short sequence);
int  icmp_ping_replied(void);

/* Send an ICMP type=3 (Destination Unreachable) reply. RFC 792 mandates
 * embedding the offending packet's IP header + first 8 bytes of payload
 * inside the ICMP body so the original sender can route the rejection back
 * to the right socket. `code` picks the sub-reason (PORT_UNREACH for
 * closed UDP ports, PROTO_UNREACH for unsupported protocols, ...). */
int  icmp_send_unreachable(ipv4_t dst, unsigned char code,
                           const void *orig_ip_packet, unsigned int orig_len);

#endif
