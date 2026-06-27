/* Network stack core: holds the local IP/MAC, drives the per-iteration
 * receive poll, and provides stubs for upper layers that haven't landed
 * yet (so ethernet.c can keep its dispatch table fully linked). */
#include "net.h"
#include "ethernet.h"
#include "e1000.h"
#include "tcp.h"
#include "dns.h"

static ipv4_t local_ip;            /* 0 until DHCP (or net_set_local_ip) fills it */
static mac_t  local_mac;

void net_init(void)
{
	if (e1000_present())
		e1000_get_mac(local_mac);
}

void   net_set_local_ip(ipv4_t ip) { local_ip = ip; }
ipv4_t net_get_local_ip(void)      { return local_ip; }

void net_get_local_mac(mac_t out)
{
	for (int i = 0; i < 6; i++)
		out[i] = local_mac[i];
}

int net_poll_once(void)
{
	/* Run the TCP retransmit timer every pump regardless of whether a frame
	 * arrived -- the wire being silent is precisely when an unacked segment
	 * needs to be resent. tcp_tick is cheap when no TCB has data in flight. */
	tcp_tick();
	/* Same idea for DNS: a dropped UDP query needs a fresh send rather than
	 * a frozen dns_ready() poll. Cheap when no query is in flight. */
	dns_tick();

	unsigned char buf[ETH_MAX_FRAME];
	unsigned int  n = e1000_poll_receive(buf, sizeof(buf));
	if (n == 0)
		return 0;
	eth_handle_frame(buf, n);
	return 1;
}

/* tcp_handle lives in tcp.c now that the state machine has landed. */
