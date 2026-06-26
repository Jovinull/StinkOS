/* Minimal DNS resolver: one in-flight A-record lookup at a time, fired at
 * the DHCP-discovered DNS server. Caller invokes dns_resolve, polls
 * dns_ready(), then reads dns_get_ip(). Designed for our own apps (HTTP
 * client, stink-pkg) -- not a full caching resolver. */
#ifndef DNS_H
#define DNS_H

#include "net.h"

/* Kick off an A-record lookup for 'name'. Replaces any in-flight request.
 * Returns 0 on success, -1 if no DNS server is configured (DHCP not bound). */
int    dns_resolve(const char *name);

/* True once the reply for the most recent dns_resolve has arrived. */
int    dns_ready(void);

/* The IPv4 address from the latest reply (network byte order); valid only
 * when dns_ready() is true. Returns 0 if no answer arrived or the lookup
 * failed (NXDOMAIN etc). */
ipv4_t dns_get_ip(void);

#endif
