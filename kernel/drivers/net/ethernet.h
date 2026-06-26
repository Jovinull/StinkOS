/* Ethernet II frame layer. Header is always 14 bytes (dst MAC + src MAC +
 * 2-byte ethertype); payload up to 1500 bytes for standard frames. Built on
 * top of e1000_send_frame; parsed frames feed arp / ip via ethertype. */
#ifndef ETHERNET_H
#define ETHERNET_H

#include "net.h"

#define ETH_ADDR_LEN     6
#define ETH_HDR_LEN      14
#define ETH_MAX_PAYLOAD  1500
#define ETH_MAX_FRAME    (ETH_HDR_LEN + ETH_MAX_PAYLOAD)

#define ETHERTYPE_IPV4   0x0800
#define ETHERTYPE_ARP    0x0806

/* Wire layout. ethertype is big-endian on the wire; callers pass host order
 * and eth_send swaps before transmitting. */
struct eth_hdr {
	unsigned char  dst[ETH_ADDR_LEN];
	unsigned char  src[ETH_ADDR_LEN];
	unsigned short ethertype;       /* network byte order on the wire */
} __attribute__((packed));

/* Build an Ethernet frame with the local MAC as source, prepend it to the
 * caller's payload, and hand it to the NIC. Returns the byte count sent,
 * or -1 if the NIC isn't initialised / payload too big. */
int  eth_send(const unsigned char dst_mac[ETH_ADDR_LEN],
              unsigned short ethertype,
              const void *payload, unsigned int payload_len);

/* Dispatch an incoming raw frame to the right upper layer based on its
 * ethertype. Frames whose ethertype we don't recognise are dropped silently. */
void eth_handle_frame(const void *frame, unsigned int len);

#endif
