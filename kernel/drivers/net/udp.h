/* UDP (RFC 768). Stateless datagram socket; the most useful upper layer for
 * DHCP, DNS, and small request/response protocols. Send is direct (caller
 * supplies dst ip + port); receive uses a tiny port-table dispatch so the
 * DHCP and DNS clients can register a callback for their reply ports. */
#ifndef UDP_H
#define UDP_H

#include "net.h"

struct udp_hdr {
	unsigned short src_port;        /* network byte order */
	unsigned short dst_port;
	unsigned short length;          /* header + payload  */
	unsigned short checksum;        /* 0 = "not computed" (legal on IPv4) */
} __attribute__((packed));

/* Send a UDP datagram. Returns bytes-on-wire on success, -1 on failure. */
int  udp_send(ipv4_t dst_ip, unsigned short dst_port,
              unsigned short src_port,
              const void *payload, unsigned int len);

/* Callback signature: invoked when a packet arrives for a registered port. */
typedef void (*udp_handler_t)(ipv4_t src_ip, unsigned short src_port,
                              const void *payload, unsigned int len);

/* Register/unregister a port handler. Up to UDP_MAX_HANDLERS bound today
 * (8 -- plenty for DHCP, DNS and a couple of app-driven sockets). */
int  udp_bind(unsigned short port, udp_handler_t cb);
void udp_unbind(unsigned short port);

/* Called by ipv4.c for IP_PROTO_UDP. */
void udp_handle(const void *payload, unsigned int len, ipv4_t src_ip);

#endif
