# StinkOS Networking

The whole stack lives in `kernel/drivers/net/`. It is intentionally
single-NIC, single-flow per direction, no TLS, and tuned to be small enough
that one engineer can keep the entire wire path in their head. This document
covers the layering, the DHCP-driven boot sequence, the TCP state machine,
and the public API a userland app uses.

## Layering

```
+---------------------------------------+   userland (TCP/UDP/DNS via syscalls)
|  apps/* (stink-pkg, ping, ...)        |
+---------------------------------------+
            int 0x80
+---------------------------------------+
|  syscalls 31..38, 43, 44              |   kernel/sys/syscall.c
+---------------------------------------+
|  tcp.c   udp.c   icmp.c   dns.c       |   L4 / L5 protocols
|  dhcp.c                               |
+---------------------------------------+
|             ipv4.c                    |   L3
+---------------------------------------+
|  arp.c          ethernet.c            |   L2
+---------------------------------------+
|             e1000.c   (PCI / MMIO)    |   driver
+---------------------------------------+
                NIC
```

`net.c` owns the boot init (`net_init`), caches the local MAC + IP, and
hosts `net_poll_once()` — the single entry point every idle loop must call.
That function drains one frame off the e1000 RX ring (if any) AND fires the
TCP retransmit timer (`tcp_tick`), so a quiet wire still gets RTO checks.

## Frame flow

**RX path** (each `net_poll_once` tick):

```
e1000_poll_receive          --> raw 1500-byte buffer
  -> eth_handle_frame       parses ethertype
       0x0806 ARP           -> arp_handle (updates cache, may auto-reply)
       0x0800 IPv4          -> ip_handle (checksum, then dispatch)
                              IP_PROTO_ICMP -> icmp_handle (echo reply)
                              IP_PROTO_UDP  -> udp_handle (port-table dispatch)
                              IP_PROTO_TCP  -> tcp_handle (state machine)
```

**TX path** (any layer that needs to emit bytes):

```
tcp_emit / udp_send / icmp_send / arp_send_request
  -> ipv4_send       (computes IPv4 header + ARP-resolves dst MAC)
       -> eth_send   (prepends ethernet header)
            -> e1000_send_frame
```

`ipv4_send` special-cases the limited broadcast address `255.255.255.255`
so DHCP can transmit before any ARP entry exists.

## DHCP boot timing

`dhcp_start()` is called from `kmain` right after `net_init`. It begins the
DORA exchange asynchronously; the rest of boot does not block.

```
T+0     | DHCPDISCOVER  (broadcast, xid=random)
T+~10ms | <-- DHCPOFFER  from a server
        | DHCPREQUEST  (broadcast, server-id = offer.siaddr)
T+~20ms | <-- DHCPACK
        | net_set_local_ip(yiaddr)
        | record subnet mask + router + DNS
```

Until the ACK lands, `SYS_NETINFO`'s `dhcp_state` field returns one of
`DHCP_INIT (0)`, `DHCP_DISCOVER (1)`, `DHCP_REQUEST (2)`; on success it
flips to `DHCP_BOUND (3)`; on timeout to `DHCP_FAILED (4)`.

Userland code that needs IP connectivity should poll `SYS_NETINFO` and wait
for `dhcp_state == DHCP_BOUND` before doing anything else. The shell's
`ifconfig` command and `ping` syscall do this implicitly: they short-circuit
to "no link" if the state has not advanced.

## ARP cache

Sixteen-entry round-robin. On insertion the oldest entry is overwritten,
not LRU — chosen for code size; production stacks should use LRU once the
table grows. Cache lookups synchronously fire an `arp_send_request` and
return -1 on miss; the caller is expected to retry from `net_poll_once` a
few ticks later.

## TCP state machine

Eight TCBs, 4 KiB rx + 4 KiB tx ring each. Stop-and-wait: only one MSS
worth of data is in flight at any time per connection. Real congestion
control + SACK + window scaling are deferred. The state graph below shows
every transition `tcp_handle()` recognises today.

```
                      passive open                       active open
                  ┌────── tcp_listen ──────┐         ┌─── tcp_connect ────┐
                  v                        v         v                    │
                LISTEN                                                    │
                  │                                                       │
            SYN in │                                                      │
                  v                                                       v
            SYN_RECEIVED                                              SYN_SENT
                  │                                                       │
       ACK in     │                                            SYN-ACK in │
                  v                                                       v
              ESTABLISHED  <───────────────  data flows  ─────────────  ESTABLISHED
                  │                                                       │
        tcp_close │           FIN in (peer closed first)        tcp_close │
                  v                  │                                    v
            FIN_WAIT_1               v                                FIN_WAIT_1
                  │              CLOSE_WAIT                              │
       ACK of FIN │                  │                       ACK of FIN │
                  v        tcp_close v                                  v
            FIN_WAIT_2              LAST_ACK                       TIME_WAIT
                  │                  │                                  │
            FIN in│           ACK in │                          (absorb retx)
                  v                  v                                  │
            TIME_WAIT             CLOSED ──────────────────────────────┘
```

A bare RST is sent back to any segment that does not match a TCB, modulo
the rule that we never RST a RST.

### Retransmission

Per TCB the kernel keeps `last_seg_ticks`, `rto_ticks` (initial 100, ceiling
6000 — PIT runs at 100 Hz so units are 10 ms), and `retries`. `tcp_tick`
checks every armed TCB on each `net_poll_once`; when the RTO expires it
resends the oldest unacked segment via `tcp_retransmit` (SYN, the in-flight
data chunk, or FIN, picked from the current state). Exponential backoff
doubles `rto_ticks` per try; after `TCP_MAX_RETRIES` (5) the connection is
dropped to CLOSED so the userland app sees the failure on its next
`SYS_SOCK_STATE`.

## DNS

Single-in-flight A-record resolver. `dns_resolve(name)` queues one UDP
query at the DHCP-supplied server (port 53). `dns_ready()` polls; the
result lands in `dns_get_ip()` once the response arrives and parses
cleanly (label compression aware). No record cache yet.

## Public API recap

* `SYS_PING(ip, timeout_ms)` — synchronous ICMP echo, returns RTT or -1.
* `SYS_NETINFO(out)` — local IP / mask / gateway / DNS / MAC / link / dhcp.
* `SYS_SOCK_CONNECT(ip, port)` -> `int handle`. Then send / recv / state /
  close. Bytes stay buffered until the FIN handshake completes.
* `SYS_SOCK_*` family is in `lib/libstink.h`; the higher-level
  `lib/libstink_http.c` shows a one-shot HTTP/1.0 GET wrapper that the
  package manager uses.

## What's intentionally missing

| Layer  | Item                              | Why deferred                                   |
|--------|-----------------------------------|------------------------------------------------|
| TCP    | Congestion control, SACK, scaling | Stop-and-wait is fine for HTTP-class flows     |
| TCP    | Out-of-order reassembly           | Discards them today; HTTP rarely fragments     |
| IPv4   | Fragmentation + reassembly        | Ethernet MTU on LAN is enough                  |
| IPv4   | Routing table                     | Single LAN; no second next-hop in scope yet    |
| TLS    | HTTPS                             | Major effort; not blocking any current use     |
| NIC    | Drivers other than e1000          | One NIC is enough to validate the stack        |
| API    | BSD-style `socket(domain, type)`  | Wrapper around the current ints would be nice  |
| L2     | IRQ-driven RX                     | Polling from `menu_run` is sufficient today    |
| IP     | IPv6                              | Out of scope until v0.5+                       |

The list mirrors `TODO.md §4`; cross every `⏳` off there as it lands here.
