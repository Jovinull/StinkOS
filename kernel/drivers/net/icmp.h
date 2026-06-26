/* ICMP (RFC 792) -- the minimum needed for `ping` to work in both directions.
 * We respond to echo requests and provide a helper to send our own. */
#ifndef ICMP_H
#define ICMP_H

#include "net.h"

#define ICMP_TYPE_ECHO_REPLY   0
#define ICMP_TYPE_ECHO_REQUEST 8

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

/* Called by ipv4.c on IP_PROTO_ICMP packets. */
void icmp_handle(const void *payload, unsigned int len, ipv4_t src_ip);

#endif
