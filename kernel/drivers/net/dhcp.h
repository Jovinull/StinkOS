/* DHCP client (RFC 2131) -- minimal four-step DORA exchange to learn an IP,
 * subnet mask, default gateway and primary DNS server. Runs over UDP/68; the
 * server side lives on a router or QEMU's slirp backend. Synchronous-ish:
 * dhcp_start kicks off DHCPDISCOVER, then the UDP callback advances the
 * state machine on each reply. dhcp_bound() polls completion. */
#ifndef DHCP_H
#define DHCP_H

#include "net.h"

enum dhcp_state {
	DHCP_INIT          = 0,    /* not started yet                          */
	DHCP_DISCOVERING   = 1,    /* DHCPDISCOVER sent, awaiting OFFER        */
	DHCP_REQUESTING    = 2,    /* DHCPREQUEST sent, awaiting ACK           */
	DHCP_BOUND         = 3,    /* lease active, IP assigned                */
	DHCP_FAILED        = 4,    /* gave up (timeouts / refusal)             */
};

/* Begin a DHCP exchange. Binds UDP port 68 for replies; safe to call once at
 * boot after the network stack is up. */
void dhcp_start(void);

/* Current client-side state. */
enum dhcp_state dhcp_get_state(void);

/* Convenience: true when DHCP_BOUND. */
int dhcp_bound(void);

/* Lease info -- valid only once dhcp_bound() returns true. */
ipv4_t dhcp_get_subnet_mask(void);
ipv4_t dhcp_get_router(void);
ipv4_t dhcp_get_dns(void);

#endif
