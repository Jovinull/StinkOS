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

/* Secondary DNS server from the OPT_DNS list, if the server offered
 * two. Returns 0 if only one was advertised (or no lease). Used as a
 * fallback when a query against the primary server times out. */
ipv4_t dhcp_get_dns2(void);

/* Pumped from net_poll_once: re-broadcasts DHCPDISCOVER / DHCPREQUEST if
 * the corresponding reply never arrived (lost packet on noisy LANs or
 * race against DHCP server warmup). Gives up after a few retries and
 * marks the lease DHCP_FAILED so userland can react instead of hanging
 * on dhcp_bound() forever. Cheap when the state machine is settled. */
void   dhcp_tick(void);

#endif
