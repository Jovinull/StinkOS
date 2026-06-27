/* Address Resolution Protocol (RFC 826) for IPv4 over Ethernet. The cache
 * is small (16 entries, LRU-ish replacement) and lives in the kernel; every
 * outgoing IPv4 packet asks arp_lookup for its destination MAC and falls
 * back to broadcasting an ARP request when the cache misses. */
#ifndef ARP_H
#define ARP_H

#include "net.h"

/* Lookup an IP -> MAC binding. Returns 1 and copies the MAC into *out on a
 * cache hit; returns 0 on miss (caller may want to arp_send_request and try
 * again later). */
int  arp_lookup(ipv4_t ip, mac_t out);

/* Broadcast an ARP request for the given IP. The reply, when it arrives,
 * lands in arp_handle and updates the cache automatically. */
void arp_send_request(ipv4_t target_ip);

/* Process an incoming ARP packet (the Ethernet payload, not the frame).
 * Updates the cache from the sender's address pair; if the packet is a
 * request for our local IP, sends back a reply. */
void arp_handle(const void *payload, unsigned int len);

/* Format the cache as ASCII into `out` ("IP MAC\n" rows, header on top).
 * Returns the byte count written (capped to `cap`). Backs SYS_ARP_INFO
 * and the shell `arp` command. */
unsigned int arp_snapshot(char *out, unsigned int cap);

#endif
