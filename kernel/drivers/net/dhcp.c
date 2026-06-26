/* Minimal DHCP client: DISCOVER -> OFFER -> REQUEST -> ACK.
 *
 * Wire layout is the 236-byte BOOTP header, then the 4-byte magic cookie
 * (0x63825363), then TLV-encoded options terminated with 0xFF. The full
 * spec has dozens of options; we only emit what we need (message type,
 * client identifier, parameter request list, server identifier on REQUEST)
 * and only parse the same handful coming back. */
#include "dhcp.h"
#include "udp.h"
#include "e1000.h"
#include "serial.h"

#define DHCP_CLIENT_PORT  68
#define DHCP_SERVER_PORT  67

#define BOOTP_REQUEST     1
#define BOOTP_REPLY       2

#define DHCP_MAGIC        0x63825363u

/* Option codes. */
#define OPT_PAD           0
#define OPT_SUBNET_MASK   1
#define OPT_ROUTER        3
#define OPT_DNS           6
#define OPT_REQUESTED_IP  50
#define OPT_LEASE_TIME    51
#define OPT_MSG_TYPE      53
#define OPT_SERVER_ID     54
#define OPT_PARAM_REQ     55
#define OPT_END           0xFF

/* DHCP message types (value of OPT_MSG_TYPE). */
#define DHCPDISCOVER      1
#define DHCPOFFER         2
#define DHCPREQUEST       3
#define DHCPDECLINE       4
#define DHCPACK           5
#define DHCPNAK           6

struct bootp_hdr {
	unsigned char  op;
	unsigned char  htype;
	unsigned char  hlen;
	unsigned char  hops;
	unsigned int   xid;
	unsigned short secs;
	unsigned short flags;
	ipv4_t         ciaddr;
	ipv4_t         yiaddr;
	ipv4_t         siaddr;
	ipv4_t         giaddr;
	unsigned char  chaddr[16];
	unsigned char  sname[64];
	unsigned char  file[128];
	unsigned int   magic;        /* network byte order: DHCP_MAGIC */
} __attribute__((packed));

static enum dhcp_state state = DHCP_INIT;
static unsigned int    our_xid = 0x4B4E5453u;   /* 'STNK' as seed */
static ipv4_t          offered_ip;
static ipv4_t          server_id;
static ipv4_t          subnet_mask;
static ipv4_t          router_ip;
static ipv4_t          dns_ip;

static void log_state(const char *name)
{
	serial_write("dhcp: ");
	serial_write(name);
	serial_putc('\n');
}

/* Encode one TLV option into buf at offset *off. Returns updated offset. */
static unsigned int put_opt(unsigned char *buf, unsigned int off,
                            unsigned char code, unsigned char len,
                            const void *data)
{
	buf[off++] = code;
	buf[off++] = len;
	const unsigned char *d = (const unsigned char *)data;
	for (unsigned int i = 0; i < len; i++)
		buf[off++] = d[i];
	return off;
}

/* Build and broadcast a DHCP message of the given type. msg_type 1 =
 * DISCOVER, 3 = REQUEST. */
static void send_dhcp(unsigned char msg_type)
{
	unsigned char pkt[576];      /* min DHCP MTU; plenty of room for options */
	for (unsigned int i = 0; i < sizeof(pkt); i++)
		pkt[i] = 0;

	struct bootp_hdr *h = (struct bootp_hdr *)pkt;
	h->op    = BOOTP_REQUEST;
	h->htype = 1;                 /* Ethernet */
	h->hlen  = 6;
	h->hops  = 0;
	h->xid   = our_xid;           /* opaque, sender-defined */
	h->secs  = 0;
	h->flags = htons(0x8000);     /* broadcast reply */
	h->ciaddr = 0;
	h->yiaddr = 0;
	h->siaddr = 0;
	h->giaddr = 0;
	mac_t mac;
	net_get_local_mac(mac);
	for (int i = 0; i < 6; i++)
		h->chaddr[i] = mac[i];
	h->magic = htonl(DHCP_MAGIC);

	unsigned int off = sizeof(*h);

	/* Option 53: message type */
	off = put_opt(pkt, off, OPT_MSG_TYPE, 1, &msg_type);

	if (msg_type == DHCPREQUEST) {
		/* Option 50: requested IP */
		off = put_opt(pkt, off, OPT_REQUESTED_IP, 4, &offered_ip);
		/* Option 54: server identifier */
		off = put_opt(pkt, off, OPT_SERVER_ID, 4, &server_id);
	}

	/* Option 55: parameter request list -- ask the server to send us
	 * subnet mask, router and DNS in its OFFER/ACK. */
	unsigned char params[3] = { OPT_SUBNET_MASK, OPT_ROUTER, OPT_DNS };
	off = put_opt(pkt, off, OPT_PARAM_REQ, sizeof(params), params);

	pkt[off++] = OPT_END;

	udp_send(0xFFFFFFFFu, DHCP_SERVER_PORT, DHCP_CLIENT_PORT, pkt, off);
}

/* Read a 32-bit IPv4 value (already in network byte order on the wire). */
static ipv4_t read_ipv4(const unsigned char *p)
{
	ipv4_t v;
	unsigned char *o = (unsigned char *)&v;
	o[0] = p[0]; o[1] = p[1]; o[2] = p[2]; o[3] = p[3];
	return v;
}

/* Scan TLV options starting at 'opts' (length 'len'). Sets out-params if a
 * recognised option is present. */
static void parse_options(const unsigned char *opts, unsigned int len,
                          unsigned char *msg_type_out)
{
	*msg_type_out = 0;
	unsigned int i = 0;
	while (i < len) {
		unsigned char code = opts[i++];
		if (code == OPT_END)
			break;
		if (code == OPT_PAD)
			continue;
		if (i >= len)
			break;
		unsigned char olen = opts[i++];
		if (i + olen > len)
			break;
		switch (code) {
		case OPT_MSG_TYPE:
			if (olen >= 1)
				*msg_type_out = opts[i];
			break;
		case OPT_SUBNET_MASK:
			if (olen == 4) subnet_mask = read_ipv4(&opts[i]);
			break;
		case OPT_ROUTER:
			if (olen >= 4) router_ip = read_ipv4(&opts[i]);
			break;
		case OPT_DNS:
			if (olen >= 4) dns_ip = read_ipv4(&opts[i]);
			break;
		case OPT_SERVER_ID:
			if (olen == 4) server_id = read_ipv4(&opts[i]);
			break;
		default:
			break;
		}
		i += olen;
	}
}

static void on_packet(ipv4_t src_ip, unsigned short src_port,
                      const void *payload, unsigned int len)
{
	(void)src_ip; (void)src_port;
	if (len < sizeof(struct bootp_hdr))
		return;

	const struct bootp_hdr *h = (const struct bootp_hdr *)payload;
	if (h->op != BOOTP_REPLY || h->xid != our_xid)
		return;
	if (ntohl(h->magic) != DHCP_MAGIC)
		return;

	unsigned char msg_type = 0;
	parse_options((const unsigned char *)payload + sizeof(*h),
	              len - sizeof(*h), &msg_type);

	if (state == DHCP_DISCOVERING && msg_type == DHCPOFFER) {
		offered_ip = h->yiaddr;
		state = DHCP_REQUESTING;
		log_state("offer received, requesting lease");
		send_dhcp(DHCPREQUEST);
		return;
	}
	if (state == DHCP_REQUESTING && msg_type == DHCPACK) {
		net_set_local_ip(h->yiaddr);
		state = DHCP_BOUND;
		log_state("bound");
		return;
	}
	if (state == DHCP_REQUESTING && msg_type == DHCPNAK) {
		state = DHCP_FAILED;
		log_state("server rejected request");
	}
}

void dhcp_start(void)
{
	if (state != DHCP_INIT)
		return;
	if (udp_bind(DHCP_CLIENT_PORT, on_packet) != 0) {
		state = DHCP_FAILED;
		return;
	}
	state = DHCP_DISCOVERING;
	log_state("sending DISCOVER");
	send_dhcp(DHCPDISCOVER);
}

enum dhcp_state dhcp_get_state(void) { return state; }
int    dhcp_bound(void)        { return state == DHCP_BOUND; }
ipv4_t dhcp_get_subnet_mask(void) { return subnet_mask; }
ipv4_t dhcp_get_router(void)      { return router_ip; }
ipv4_t dhcp_get_dns(void)         { return dns_ip; }
