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

/* Flat snapshot of the host's network configuration, filled by the
 * SYS_NETINFO syscall so a ring3 app can render it. The layout is a stable
 * ABI contract: apps/libstink.h mirrors this struct byte-for-byte (24 bytes,
 * 4-aligned, no padding). All ipv4_t fields are in network byte order. */
struct net_info {
	ipv4_t        ip;            /* 0  assigned address (0 until bound)   */
	ipv4_t        mask;          /* 4  subnet mask                        */
	ipv4_t        gateway;       /* 8  default gateway / router           */
	ipv4_t        dns;           /* 12 primary DNS server                 */
	unsigned char mac[6];        /* 16 NIC hardware address               */
	unsigned char dhcp_state;    /* 22 enum dhcp_state value              */
	unsigned char link_up;       /* 23 1 if the NIC is present/initialised*/
};

/* Local-host configuration. The MAC comes from the NIC; the IP is set by the
 * DHCP client at boot (or by hand for testing). Both are zero until then. */
void   net_set_local_ip(ipv4_t ip);
ipv4_t net_get_local_ip(void);
void   net_get_local_mac(mac_t out);

/* Returns 1 if `addr` looks like a routable unicast peer: not the
 * unspecified address (0.0.0.0), not the limited broadcast
 * (255.255.255.255), not loopback (127/8), and not class-D multicast
 * (224.0.0.0/4). Used by every place that wants to refuse sending to,
 * or processing a peer from, an address that can only be a spoof or
 * misconfiguration. Address is in network byte order, so the high octet
 * lives in the low 8 bits of the integer. */
static inline int ipv4_is_unicast(ipv4_t addr)
{
	if (addr == 0 || addr == 0xFFFFFFFFu)
		return 0;
	if ((addr & 0x000000FFu) == 0x0000007Fu)   /* 127/8  loopback */
		return 0;
	if ((addr & 0x000000F0u) == 0x000000E0u)   /* 224/4  multicast */
		return 0;
	return 1;
}

/* Pumps one frame through the receive path: poll the NIC, dispatch by
 * ethertype, return. Returns 1 if a frame was processed, 0 if the RX ring
 * was empty. Designed to be called from any idle/poll loop. */
int    net_poll_once(void);

/* Initialise the network stack: caches the NIC's MAC for layer-2 emit. Call
 * once after e1000_init succeeds. */
void   net_init(void);

#endif
