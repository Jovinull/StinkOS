/* IPv4 layer (RFC 791) on top of Ethernet+ARP. Provides outgoing packet
 * assembly (header build, checksum, ARP resolution, MTU bounds) and the
 * incoming dispatch path that drives ICMP / UDP / TCP.
 *
 * Fragmentation is intentionally not handled today: outgoing packets are
 * bounded to the standard Ethernet payload minus the IP header, and any
 * incoming packet with a non-zero fragment offset or the MF bit set is
 * dropped silently. Re-introducing it is a self-contained extension.
 */
#ifndef IPV4_H
#define IPV4_H

#include "net.h"

#define IP_PROTO_ICMP 1
#define IP_PROTO_TCP  6
#define IP_PROTO_UDP  17

#define IPV4_DEFAULT_TTL 64

/* 20-byte IPv4 header, no options (the typical and only form we emit). */
struct ipv4_hdr {
	unsigned char  ver_ihl;          /* 0x45 = IPv4, IHL = 5 dwords      */
	unsigned char  tos;
	unsigned short total_length;     /* big-endian: header + payload     */
	unsigned short id;
	unsigned short flags_fragoff;
	unsigned char  ttl;
	unsigned char  protocol;
	unsigned short checksum;
	ipv4_t         src_ip;           /* network byte order               */
	ipv4_t         dst_ip;
} __attribute__((packed));

/* Build + send an IPv4 packet. ARP-resolves dst on the wire (drops the
 * packet and triggers an ARP request on cache miss), bumps the IP id
 * counter, fills the checksum. Returns the bytes-on-wire on success, -1
 * on failure (no NIC, payload too big, etc.). */
int  ipv4_send(ipv4_t dst, unsigned char protocol,
               const void *payload, unsigned int payload_len);

/* Computes the one's-complement 16-bit Internet checksum used by IP, ICMP,
 * UDP and TCP. Works directly on network-byte-order data; result is also
 * network byte order. */
unsigned short ipv4_checksum(const void *data, unsigned int len);

/* Called by ethernet.c for ETHERTYPE_IPV4 frames. Validates header, dispatch
 * by protocol field. */
void ip_handle(const void *payload, unsigned int len);

#endif
