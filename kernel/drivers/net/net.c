/* Network stack core: holds the local IP/MAC, drives the per-iteration
 * receive poll, and provides stubs for upper layers that haven't landed
 * yet (so ethernet.c can keep its dispatch table fully linked). */
#include "net.h"
#include "ethernet.h"
#include "e1000.h"
#include "tcp.h"
#include "dns.h"
#include "dhcp.h"
#include "proc.h"

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
	/* And for DHCP: a lost DISCOVER / REQUEST used to leave the client
	 * stuck in DISCOVERING forever; the tick retransmits with a bounded
	 * retry budget. No-op once a lease is bound. */
	dhcp_tick();

	/* Static: the stack is 8 KiB and the handlers below nest several
	 * more frame-sized buffers. Safe because nothing in the network stack
	 * runs in interrupt context or blocks: one caller at a time. */
	static unsigned char buf[ETH_MAX_FRAME];
	unsigned int  n = e1000_poll_receive(buf, sizeof(buf));
	if (n == 0)
		return 0;
	eth_handle_frame(buf, n);
	return 1;
}

/* ---- network thread -------------------------------------------------------
 *
 * All receive processing and all protocol timers run here, in process
 * context. The NIC interrupt only wakes this thread. Because the kernel
 * does not preempt itself, the thread and any syscall that enters the
 * stack (tcp_send, net_poll_once, ...) are mutually exclusive without
 * locks, and no interrupt handler ever touches a TCB, the ARP cache or a
 * NIC ring. */
static struct proc *net_thread;

static void net_thread_main(void)
{
	for (;;) {
		int budget = 64;                   /* frames per wake-up, then let others run */
		while (budget-- > 0 && net_poll_once())
			;
		/* Woken by the NIC interrupt; the timeout keeps the protocol
		 * timers (retransmit, DHCP, DNS) ticking on a quiet wire. */
		proc_block(2);
	}
}

void net_start(void)
{
	if (!e1000_present() || net_thread)
		return;
	net_thread = proc_kthread_create("knet", net_thread_main);
	if (net_thread)
		net_thread->priority = 8;          /* ahead of ordinary processes (16) */
}

void net_irq_wake(void)
{
	if (net_thread)
		proc_wake(net_thread);
}

/* tcp_handle lives in tcp.c now that the state machine has landed. */
