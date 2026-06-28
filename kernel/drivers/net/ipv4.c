/* IPv4 layer: send + dispatch. The send path builds a 20-byte header on the
 * stack, resolves the destination MAC via ARP, and ships the assembled
 * datagram down to eth_send. The receive path strips the header, validates
 * the checksum, and routes by protocol to ICMP / UDP / TCP. */
#include "ipv4.h"
#include "arp.h"
#include "ethernet.h"
#include "dhcp.h"           /* mask + router lookup for routing decisions */
#include "interrupts.h"     /* pit_ticks() for reassembly TTL */
#include "interrupts.h"     /* pit_ticks() for fragment timeout            */

/* Forward declarations for upper-layer dispatchers. icmp / udp / tcp .c
 * files override these temporary stubs in net.c when they land. */
void icmp_handle(const void *payload, unsigned int len,
                 ipv4_t src_ip, ipv4_t dst_ip);
void udp_handle(const void *payload, unsigned int len, ipv4_t src_ip);
void tcp_handle(const void *payload, unsigned int len, ipv4_t src_ip);

#define IP_FLAG_MF       0x2000u    /* more fragments */
#define IP_FRAGOFF_MASK  0x1FFFu

static unsigned short ip_id_counter = 1;

/* ---- Fragment reassembly --------------------------------------------------
 *
 * Two-slot reassembly pool. Each slot accepts up to REASM_MAXLEN bytes of a
 * single IPv4 datagram and tracks coverage at 8-byte granularity (the same
 * unit IPv4 uses for fragment offsets) via a bitmap. Slots time out after
 * REASM_TTL ticks (100 Hz PIT) so a missing tail cannot pin memory forever.
 *
 * Out of scope for now: > 8 KiB datagrams, more than 2 concurrent flows,
 * overlapping-fragment policy (we accept the latest write -- the same as
 * traditional BSD before the teardrop hardening).
 */
#define REASM_SLOTS   2
#define REASM_MAXLEN  8192u
#define REASM_UNIT    8u            /* IPv4 fragoff unit, bytes */
#define REASM_BMAP    ((REASM_MAXLEN / REASM_UNIT + 7u) / 8u)
#define REASM_TTL     3000u         /* 30 s at 100 Hz */

struct reasm_slot {
	int             in_use;
	ipv4_t          src_ip;
	ipv4_t          dst_ip;
	unsigned short  ip_id;
	unsigned char   protocol;
	unsigned int    last_tick;
	unsigned int    total_len;        /* set when MF=0 fragment seen, else 0 */
	unsigned char   bitmap[REASM_BMAP];
	unsigned char   buf[REASM_MAXLEN];
};

static struct reasm_slot reasm_pool[REASM_SLOTS];

static void reasm_clear(struct reasm_slot *r)
{
	r->in_use     = 0;
	r->total_len  = 0;
	r->last_tick  = 0;
	for (unsigned int i = 0; i < REASM_BMAP; i++)
		r->bitmap[i] = 0;
}

static struct reasm_slot *reasm_find_or_alloc(ipv4_t src, ipv4_t dst,
                                              unsigned short id,
                                              unsigned char proto)
{
	unsigned int now = pit_ticks();
	int free_idx = -1;
	int stale_idx = -1;

	for (int i = 0; i < REASM_SLOTS; i++) {
		struct reasm_slot *r = &reasm_pool[i];
		if (r->in_use &&
		    r->src_ip   == src && r->dst_ip == dst &&
		    r->ip_id    == id  && r->protocol == proto) {
			r->last_tick = now;
			return r;
		}
		if (!r->in_use && free_idx < 0)
			free_idx = i;
		else if (r->in_use && (unsigned int)(now - r->last_tick) > REASM_TTL)
			stale_idx = i;
	}

	int pick = (free_idx >= 0) ? free_idx : stale_idx;
	if (pick < 0)
		return 0;                          /* table full, no recyclable slot */

	struct reasm_slot *r = &reasm_pool[pick];
	reasm_clear(r);
	r->in_use    = 1;
	r->src_ip    = src;
	r->dst_ip    = dst;
	r->ip_id     = id;
	r->protocol  = proto;
	r->last_tick = now;
	return r;
}

/* Mark units [unit_start, unit_start+units) as received in the bitmap. */
static void reasm_mark_units(struct reasm_slot *r, unsigned int unit_start,
                             unsigned int units)
{
	for (unsigned int u = unit_start; u < unit_start + units; u++) {
		if (u >= REASM_MAXLEN / REASM_UNIT)
			break;
		r->bitmap[u >> 3] |= (unsigned char)(1u << (u & 7u));
	}
}

/* Returns 1 if every unit needed to cover total_len is present. Only safe to
 * call once total_len is known (the MF=0 fragment has been seen). */
static int reasm_complete(const struct reasm_slot *r)
{
	if (r->total_len == 0)
		return 0;
	unsigned int needed = (r->total_len + REASM_UNIT - 1u) / REASM_UNIT;
	for (unsigned int u = 0; u < needed; u++) {
		if (!(r->bitmap[u >> 3] & (unsigned char)(1u << (u & 7u))))
			return 0;
	}
	return 1;
}

unsigned short ipv4_checksum(const void *data, unsigned int len)
{
	const unsigned char *p = (const unsigned char *)data;
	unsigned int sum = 0;

	for (unsigned int i = 0; i + 1 < len; i += 2)
		sum += ((unsigned int)p[i] << 8) | p[i + 1];
	if (len & 1)
		sum += (unsigned int)p[len - 1] << 8;
	while (sum >> 16)
		sum = (sum & 0xFFFFu) + (sum >> 16);
	return htons((unsigned short)(~sum & 0xFFFFu));
}

int ipv4_send(ipv4_t dst, unsigned char protocol,
              const void *payload, unsigned int payload_len)
{
	if (payload_len > ETH_MAX_PAYLOAD - sizeof(struct ipv4_hdr))
		return -1;

	mac_t dst_mac;
	if (dst == 0xFFFFFFFFu) {
		/* 255.255.255.255 (limited broadcast): no ARP, dst MAC is the
		 * Ethernet broadcast address. DHCPDISCOVER and similar
		 * bootstrap traffic rely on this path. */
		for (int i = 0; i < 6; i++)
			dst_mac[i] = 0xFF;
	} else {
		/* Routing decision: if dst is off our subnet and DHCP gave us a
		 * default gateway, forward through that gateway. The IP header
		 * still carries the original dst_ip -- only the layer-2 ARP
		 * lookup is redirected to the router's MAC. */
		ipv4_t local  = net_get_local_ip();
		ipv4_t mask   = dhcp_get_subnet_mask();
		ipv4_t router = dhcp_get_router();
		ipv4_t arp_target = dst;
		if (mask != 0 && router != 0 &&
		    ((local & mask) != (dst & mask))) {
			arp_target = router;
		}
		if (!arp_lookup(arp_target, dst_mac)) {
			/* No L2 binding yet; kick off resolution and let the
			 * caller retry on a future tick. */
			arp_send_request(arp_target);
			return -1;
		}
	}

	unsigned char  packet[ETH_MAX_PAYLOAD];
	struct ipv4_hdr *h = (struct ipv4_hdr *)packet;

	h->ver_ihl        = 0x45;                /* IPv4, IHL = 5 dwords */
	h->tos            = 0;
	h->total_length   = htons((unsigned short)(sizeof(*h) + payload_len));
	h->id             = htons(ip_id_counter++);
	h->flags_fragoff  = 0;
	h->ttl            = IPV4_DEFAULT_TTL;
	h->protocol       = protocol;
	h->checksum       = 0;
	h->src_ip         = net_get_local_ip();
	h->dst_ip         = dst;
	h->checksum       = ipv4_checksum(h, sizeof(*h));

	const unsigned char *pld = (const unsigned char *)payload;
	for (unsigned int i = 0; i < payload_len; i++)
		packet[sizeof(*h) + i] = pld[i];

	return eth_send(dst_mac, ETHERTYPE_IPV4, packet, sizeof(*h) + payload_len);
}

void ip_handle(const void *payload, unsigned int len)
{
	if (len < sizeof(struct ipv4_hdr))
		return;

	const struct ipv4_hdr *h = (const struct ipv4_hdr *)payload;

	/* IPv4 only, no options. ihl < 5 is malformed. */
	if ((h->ver_ihl >> 4) != 4)
		return;
	unsigned int ihl = (h->ver_ihl & 0x0F) * 4;
	if (ihl < sizeof(struct ipv4_hdr) || ihl > len)
		return;

	unsigned int total = ntohs(h->total_length);
	if (total > len || total < ihl)
		return;

	/* Drop if not for us (and not broadcast). */
	ipv4_t local = net_get_local_ip();
	if (h->dst_ip != local && h->dst_ip != 0xFFFFFFFFu && local != 0)
		return;

	/* RFC 1812 §5.3.7 / "Martian" address filter: drop packets whose
	 * source claims to be our own address (anti-spoof so an attacker
	 * cannot loop something back into our state machines), the limited
	 * broadcast, loopback, or class-D multicast as a source. src=0 is
	 * allowed because DHCP OFFER/ACK legitimately arrives that way
	 * before our lease binds. */
	if (local != 0 && h->src_ip == local)
		return;
	if (h->src_ip != 0 && !ipv4_is_unicast(h->src_ip))
		return;

	/* Verify the header checksum. */
	if (ipv4_checksum(h, ihl) != 0)
		return;

	/* RFC 1812 §5.3.5 / RFC 7126: source-routed packets (LSRR opt 131,
	 * SSRR opt 137) let an attacker dictate the return path. Every
	 * modern host drops them on receive. The IPv4 options sit between
	 * the fixed 20-byte header and ihl*4; walk them looking for the
	 * dangerous kinds. */
	if (ihl > sizeof(struct ipv4_hdr)) {
		const unsigned char *opts =
		    (const unsigned char *)payload + sizeof(struct ipv4_hdr);
		unsigned int olen = ihl - sizeof(struct ipv4_hdr);
		for (unsigned int i = 0; i < olen; ) {
			unsigned char kind = opts[i];
			if (kind == 0) break;          /* end of options */
			if (kind == 1) { i++; continue; } /* NOP */
			if (i + 1 >= olen) break;
			unsigned char optlen = opts[i + 1];
			if (optlen < 2 || i + optlen > olen) break;
			if (kind == 131 || kind == 137) /* LSRR / SSRR */
				return;
			i += optlen;
		}
	}

	const unsigned char *pld = (const unsigned char *)payload + ihl;
	unsigned int         pld_len = total - ihl;

	unsigned short ff       = ntohs(h->flags_fragoff);
	unsigned int   frag_off = (ff & IP_FRAGOFF_MASK) * REASM_UNIT;
	int            more     = (ff & IP_FLAG_MF) != 0;

	if (frag_off == 0 && !more) {
		/* Whole datagram in one packet -- common path. */
		switch (h->protocol) {
		case IP_PROTO_ICMP: icmp_handle(pld, pld_len, h->src_ip, h->dst_ip); break;
		case IP_PROTO_UDP:  udp_handle(pld, pld_len, h->src_ip);  break;
		case IP_PROTO_TCP:  tcp_handle(pld, pld_len, h->src_ip);  break;
		default: break;
		}
		return;
	}

	/* Fragmented: copy into the reassembly slot, mark coverage, dispatch when
	 * every unit needed for total_len is filled. */
	if (frag_off + pld_len > REASM_MAXLEN)
		return;                                 /* too big for our slot */

	struct reasm_slot *r = reasm_find_or_alloc(h->src_ip, h->dst_ip,
	                                           h->id, h->protocol);
	if (!r)
		return;

	unsigned int unit_start = frag_off / REASM_UNIT;
	unsigned int units      = (pld_len + REASM_UNIT - 1u) / REASM_UNIT;

	/* RFC 5722 / RFC 8200: drop the whole datagram if any incoming
	 * fragment overlaps a unit we've already received. The Teardrop
	 * class of attacks relies on the overwrite of in-place reassembly
	 * bytes; silently discarding the entire slot frees the resources
	 * and forces the legitimate sender (if any) to start over. */
	for (unsigned int u = unit_start; u < unit_start + units; u++) {
		if (u >= REASM_MAXLEN / REASM_UNIT)
			break;
		if (r->bitmap[u >> 3] & (unsigned char)(1u << (u & 7u))) {
			reasm_clear(r);
			return;
		}
	}

	for (unsigned int i = 0; i < pld_len; i++)
		r->buf[frag_off + i] = pld[i];

	reasm_mark_units(r, unit_start, units);

	if (!more)
		r->total_len = frag_off + pld_len;

	if (!reasm_complete(r))
		return;

	/* Snapshot before freeing the slot so the dispatch sees stable bytes.
	 * `dgram_len` is the reassembled datagram size, distinct from the
	 * ip_handle param `len` (size of the single incoming fragment). */
	unsigned char  proto     = r->protocol;
	ipv4_t         src       = r->src_ip;
	ipv4_t         dst       = r->dst_ip;
	unsigned int   dgram_len = r->total_len;
	const unsigned char *full = r->buf;

	switch (proto) {
	case IP_PROTO_ICMP: icmp_handle(full, dgram_len, src, dst); break;
	case IP_PROTO_UDP:  udp_handle(full, dgram_len, src);  break;
	case IP_PROTO_TCP:  tcp_handle(full, dgram_len, src);  break;
	default: break;
	}
	reasm_clear(r);
}
