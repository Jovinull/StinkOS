/* ARP cache + request/reply handling. Cache is a fixed-size table walked
 * linearly; collisions overwrite the oldest entry (insertion-time round-
 * robin). 16 entries is plenty for a single-LAN host that talks to one or
 * two peers + a default gateway. */
#include "arp.h"
#include "ethernet.h"

#define ARP_HW_ETHERNET 1
#define ARP_PROTO_IPV4  0x0800
#define ARP_REQUEST     1
#define ARP_REPLY       2

struct arp_packet {
	unsigned short hw_type;
	unsigned short proto_type;
	unsigned char  hw_size;
	unsigned char  proto_size;
	unsigned short opcode;
	unsigned char  sender_mac[6];
	unsigned int   sender_ip;       /* network byte order */
	unsigned char  target_mac[6];
	unsigned int   target_ip;       /* network byte order */
} __attribute__((packed));

#define ARP_CACHE_SIZE 16

struct arp_entry {
	ipv4_t ip;
	mac_t  mac;
	int    valid;
};

static struct arp_entry cache[ARP_CACHE_SIZE];
static unsigned int      cache_next;        /* round-robin replacement slot */

static const unsigned char broadcast_mac[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };

static void cache_insert(ipv4_t ip, const mac_t mac)
{
	/* Update in place if we already track this IP. */
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].valid && cache[i].ip == ip) {
			for (int k = 0; k < 6; k++)
				cache[i].mac[k] = mac[k];
			return;
		}
	}
	/* Find a free slot, or wrap into the round-robin cursor. */
	int slot = -1;
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (!cache[i].valid) { slot = i; break; }
	}
	if (slot < 0) {
		slot = (int)cache_next;
		cache_next = (cache_next + 1) % ARP_CACHE_SIZE;
	}
	cache[slot].ip    = ip;
	for (int k = 0; k < 6; k++)
		cache[slot].mac[k] = mac[k];
	cache[slot].valid = 1;
}

int arp_lookup(ipv4_t ip, mac_t out)
{
	for (int i = 0; i < ARP_CACHE_SIZE; i++) {
		if (cache[i].valid && cache[i].ip == ip) {
			for (int k = 0; k < 6; k++)
				out[k] = cache[i].mac[k];
			return 1;
		}
	}
	return 0;
}

void arp_send_request(ipv4_t target_ip)
{
	struct arp_packet p;
	mac_t my_mac;
	net_get_local_mac(my_mac);

	p.hw_type    = htons(ARP_HW_ETHERNET);
	p.proto_type = htons(ARP_PROTO_IPV4);
	p.hw_size    = 6;
	p.proto_size = 4;
	p.opcode     = htons(ARP_REQUEST);
	for (int i = 0; i < 6; i++)
		p.sender_mac[i] = my_mac[i];
	p.sender_ip = net_get_local_ip();
	for (int i = 0; i < 6; i++)
		p.target_mac[i] = 0;        /* unknown */
	p.target_ip = target_ip;

	eth_send(broadcast_mac, ETHERTYPE_ARP, &p, sizeof(p));
}

static void arp_send_reply(const struct arp_packet *req)
{
	struct arp_packet p;
	mac_t my_mac;
	net_get_local_mac(my_mac);

	p.hw_type    = htons(ARP_HW_ETHERNET);
	p.proto_type = htons(ARP_PROTO_IPV4);
	p.hw_size    = 6;
	p.proto_size = 4;
	p.opcode     = htons(ARP_REPLY);
	for (int i = 0; i < 6; i++)
		p.sender_mac[i] = my_mac[i];
	p.sender_ip = net_get_local_ip();
	for (int i = 0; i < 6; i++)
		p.target_mac[i] = req->sender_mac[i];
	p.target_ip = req->sender_ip;

	eth_send(req->sender_mac, ETHERTYPE_ARP, &p, sizeof(p));
}

void arp_handle(const void *payload, unsigned int len)
{
	if (len < sizeof(struct arp_packet))
		return;

	const struct arp_packet *p = (const struct arp_packet *)payload;

	/* Only handle IPv4-over-Ethernet ARP. */
	if (ntohs(p->hw_type) != ARP_HW_ETHERNET ||
	    ntohs(p->proto_type) != ARP_PROTO_IPV4 ||
	    p->hw_size != 6 || p->proto_size != 4)
		return;

	/* Every valid ARP packet teaches us the sender's binding. */
	cache_insert(p->sender_ip, p->sender_mac);

	/* Requests targeting our IP get a reply. */
	if (ntohs(p->opcode) == ARP_REQUEST &&
	    p->target_ip == net_get_local_ip() &&
	    net_get_local_ip() != 0) {
		arp_send_reply(p);
	}
}
