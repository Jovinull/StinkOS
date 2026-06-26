/* Common network-stack types + byte-order helpers. Sits above the e1000 NIC
 * driver and below ethernet/arp/ip/udp/tcp. Every layer agrees on:
 *
 *   - mac_t  : 6-byte hardware address (raw bytes on the wire)
 *   - ipv4_t : 32-bit IPv4 address stored in network byte order (big-endian)
 *
 * Wire structures use htons/htonl + __attribute__((packed)) so we never have
 * to memcpy out of misaligned descriptors. */
#ifndef NET_H
#define NET_H

typedef unsigned int  ipv4_t;       /* always network byte order */
typedef unsigned char mac_t[6];

/* 16-bit byte swap (network <-> host on a little-endian i386). */
static inline unsigned short htons(unsigned short v)
{
	return (unsigned short)((v << 8) | (v >> 8));
}
static inline unsigned short ntohs(unsigned short v) { return htons(v); }

/* 32-bit byte swap. */
static inline unsigned int htonl(unsigned int v)
{
	return ((v & 0x000000FFu) << 24) |
	       ((v & 0x0000FF00u) <<  8) |
	       ((v & 0x00FF0000u) >>  8) |
	       ((v & 0xFF000000u) >> 24);
}
static inline unsigned int ntohl(unsigned int v) { return htonl(v); }

/* Compose an IPv4 address from four octets in host order (e.g. 192,168,1,1).
 * Returns the value already in network byte order, ready to drop into wire
 * structures. */
static inline ipv4_t ipv4(unsigned char a, unsigned char b,
                          unsigned char c, unsigned char d)
{
	return htonl(((unsigned int)a << 24) | ((unsigned int)b << 16) |
	             ((unsigned int)c <<  8) |  (unsigned int)d);
}

/* Local-host configuration. The MAC comes from the NIC; the IP is set by the
 * DHCP client at boot (or by hand for testing). Both are zero until then. */
void   net_set_local_ip(ipv4_t ip);
ipv4_t net_get_local_ip(void);
void   net_get_local_mac(mac_t out);

/* Pumps one frame through the receive path: poll the NIC, dispatch by
 * ethertype, return. Returns 1 if a frame was processed, 0 if the RX ring
 * was empty. Designed to be called from any idle/poll loop. */
int    net_poll_once(void);

/* Initialise the network stack: caches the NIC's MAC for layer-2 emit. Call
 * once after e1000_init succeeds. */
void   net_init(void);

#endif
