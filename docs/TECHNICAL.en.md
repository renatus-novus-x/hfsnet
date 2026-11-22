# hfsnet technical details

[English](TECHNICAL.en.md) | [日本語](TECHNICAL.md)

Protocol and driver implementation details.
For usage, see the [README](../README.en.md).

## File protocol

Only two files live in the shared folder. Both are binary, and multi-byte
values are big-endian (network byte order).

- `HFSREQ.BIN` — guest → host request (written by the guest)
- `HFSRES.BIN` — host → guest response (written by the host; written under a
  temporary name and then renamed into place, so the guest never reads a
  partially written response)

### Request (HFSREQ.BIN, 20-byte header + payload)

| Offset | Size | Contents |
| --- | --- | --- |
| 0 | 2 | Magic `'H' 'N'` |
| 2 | 1 | Operation code (see below) |
| 3 | 1 | Socket number (guest-side slot 0–31) |
| 4 | 4 | Sequence number (incremented by the guest per request; used to match responses) |
| 8 | 2 | Port number (used by CONNECT / BIND; the destination port for UDP SEND) |
| 10 | 2 | Auxiliary argument (socket type for OPEN (0 = SOCK_STREAM, 1 = SOCK_DGRAM, 2 = SOCK_RAW), LISTEN backlog, ACCEPT destination slot, SHUTDOWN how) |
| 12 | 4 | IPv4 address (used by CONNECT / BIND; the destination address for UDP SEND) |
| 16 | 4 | Payload length (for RECV, the receive limit) |
| 20 | variable | Payload (SEND data, the host name for RESOLVE, etc.) |

### Response (HFSRES.BIN, 24-byte header + payload)

| Offset | Size | Contents |
| --- | --- | --- |
| 0 | 2 | Magic `'H' 'N'` |
| 2 | 1 | Operation code (copy of the request) |
| 3 | 1 | Socket number (copy of the request) |
| 4 | 4 | Sequence number (copy of the request) |
| 8 | 4 | Result (meaning depends on the operation; see below) |
| 12 | 2 | Port number (the peer's port for ACCEPT; the source port for UDP RECV) |
| 14 | 2 | Reserved (0) |
| 16 | 4 | Payload length (for STATE, the number of bytes available to receive) |
| 20 | 4 | Auxiliary value (state code for STATE, IPv4 address for RESOLVE, the peer's IPv4 address for ACCEPT, the source IPv4 address for UDP RECV) |
| 24 | variable | Payload (data for RECV) |

### Operation codes and the meaning of the result

| Code | Name | Meaning of the result |
| --- | --- | --- |
| 1 | OPEN | 0 = success, -1 = failure |
| 2 | CLOSE | 0 = success |
| 3 | CONNECT | 0 = connected, 1 = in progress, -1 = failure |
| 4 | SEND | Number of bytes sent, -2 = cannot send now, -1 = failure |
| 5 | RECV | Number of bytes received, 0 = peer closed (EOF), -2 = no data yet, -1 = failure |
| 6 | STATE | State code (see below). len = bytes available to receive, aux = bytes that can be sent (approximate) |
| 7 | RESOLVE | 0 = success (IPv4 address in aux), -1 = failure |
| 8 | BIND | 0 = success, -1 = failure |
| 9 | LISTEN | 0 = success, -1 = failure |
| 10 | ACCEPT | 0 = success, -2 = no connection yet, -1 = failure |
| 11 | SHUTDOWN | 0 = success, -1 = failure. how (auxiliary argument): 0 = receive side, 1 = send side, 2 = both |
| 12 | HOSTINFO | 0 = success (the host's primary IPv4 address in aux, or 0 if unavailable). Issued once by the guest driver at install time to check that the host application is responding and to display its IP address. Older hosts reply -1 as for any unknown operation code |

State codes: 0 CLOSED / 1 LISTEN / 2 SYN_SENT / 3 SYN_RECEIVED /
4 ESTABLISHED / 5 CLOSE_WAIT / 6 FIN_WAIT / 7 TIME_WAIT / 8 CLOSING

### Synchronization rules

- Only one request is in flight at a time (enough for Human68k applications,
  which basically call the driver from a single flow of control)
- The guest writes and closes the request, then keeps re-opening the
  response file until it finds a response whose sequence number matches
- The host processes only requests whose sequence number differs from the
  last one it handled; a partially written request (header length not
  matching the actual file length) is read again later

## TI functions implemented by the guest driver

Numbering follows the function table in Keisoku Giken's tcpipdrv.h
(the order of the `_TI_` entries).

Implemented: get_version / socket / bind / listen / accept / connect /
read_s / write_s / recvfrom / sendto / close_s / shutdown / socklen /
getsockname / getpeername / sockkick / usesock / recvline / sendline /
rrecvchar / recvchar / usflush / seteol / sockmode / setflush /
psocket / sockerr / sockstate / sock_top / ntoa_sock / gethostbyname /
get_MIB / add_arp_table / del_arp_table / search_arp_table /
get_arp_table_top / arp_request / get_iface_list / get_new_iface /
link_new_iface / rt_top / rt_lookup / rt_lookupb / rt_drop / rt_add /
dns_add / dns_drop / dns_get / set_domain_name / get_domain_name / rip

The management functions (interfaces / routes / DNS / ARP / RIP) are
implemented as a display-and-settings ledger, because the actual
traffic lives in the host's stack. There are two interfaces: lp0
(127.0.0.1) and hf0 (the host's IP address, learned from the HOSTINFO
reply at install time; its netmask is the classful default), which
makes ifconfig and netstat -i / -r work. Only three routes are
reported: the default route, hf0's subnet, and 127.0.0.0/8.
inetdconf's default router, DNS servers, domain name, and RIP flag
are kept guest-side and reflected in its display, but they do not
affect actual traffic. There is no ARP table (get_arp_table_top
returns a pointer to an array of 17 empty buckets, as the ABI
requires).

The pointer-returning lookup functions (gethostbyaddr /
getnetbyname / getnetbyaddr / getservbyname / getservbyport /
getprotobyname / getprotobynumber) return NULL, not -1, when the
lookup fails. Any other unimplemented function returns -1. Limitations:

- Socket types supported are TCP (SOCK_STREAM), UDP (SOCK_DGRAM),
  and RAW (SOCK_RAW)
- RAW carries ICMP (Internet Control Message Protocol) echo, used by
  ping.x. The actual exchange is handled by the GUI host; the Python
  host refuses RAW opens, so use the GUI host for ping.x (ping.x also
  needs FLOAT2.X resident for its statistics display)
- The address arguments of sendto / recvfrom are honored for UDP
  (for TCP the socket is treated as connected)
- Flags such as MSG_PEEK are ignored
- getsockname returns the actual local address and port established
by bind / connect (0.0.0.0:0 if neither has happened)

The driver's resident layout follows the same convention as real
inetd.x-compatible drivers (such as yunkya2's joynetd): the program image
starts with the `"TCP/IP"` signature, the entry point is right after it
(at offset 8), it is called with the function number in D0 and a pointer
to the argument array in A0, and the result comes back in D0. The socket
state structures (the usock array) are published via sock_top, which also
matches libsocket's convention of reading and writing the non-blocking
flag directly.

## Building

Guest side (using elf2x68k):

```sh
cd guest
export PATH=<elf2x68k bin directory>:$PATH
make        # produces hfsnet.x
```

The GUI host is built with CMake in `gui/`.
The Python host needs no build.
