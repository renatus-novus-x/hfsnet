[English](README.en.md) | [日本語](README.md)

# hfsnet — an inetd.x-compatible driver over a shared folder

hfsnet is an experimental implementation that gives Human68k a Keisoku Giken
**inetd.x-compatible API** (the TCP/IP driver calling convention, the so-called
TI entry) using nothing but a folder that the guest can read and write from
the host side (such as XEiJ's **HFS**, XM6 TypeG's **Windrv**, and
WebX68k's **HostFS**).

No emulator-specific features are used — only ordinary file reads and writes.

- Guest side: a resident driver, `hfsnet.x` (built with elf2x68k). It
  translates TI calls from applications into request/response file exchanges
  inside the shared folder.
- Host side: a console app, `hfsnethost.py` (Python 3, standard library
  only). It watches the request file, operates real TCP sockets, and writes
  the results into the response file.

Receive notification is done by polling — the driver periodically re-opens
the response file — rather than by interrupts (blocking calls wait in a loop
inside the driver).

## In action

All of these were recorded on XEiJ.

httpclient.x running (left: guest, right: the host-side hfsnethost.py log):

![httpclient.x with the host-side log](httpclient-host.gif)

iperf.x in client mode (left: guest, right: host side):

![iperf.x in client mode](iperf-client.gif)

iperf.x in server mode (left: guest, right: host side; played at 2x speed):

![iperf.x in server mode](iperf-server.gif)

sntp.x running (left: guest, right: host side; fetching the time from an
SNTP server over UDP):

![sntp.x running](sntp-host.gif)

telnet.x running (left: guest, right: host side; the TCPPACK telnet client
connecting to a test server, exchanging echoes, and disconnecting):

![telnet.x running](telnet-host.gif)

WebXpressionPlus running (left: guest, right: host side; a web browser
fetching and displaying a test page, then following a link to a second
page):

![WebXpressionPlus running](webxpression-host.gif)

nc.x running (left: guest, right: host side; netcat-x68k connecting to an
echo server, exchanging data, then exiting back to the prompt when the
peer disconnects):

![nc.x running](nc-host.gif)

## Requirements

- A shared folder that both the guest and the host can read and write
  (writes must reach the real file when the file is closed)
- `hfsnethost.py` running on the host
- Guest applications that use an inetd.x-compatible API, such as elf2x68k's
  libsocket

Verified environments: five. On each of XEiJ (HFS), XM6 TypeG (Windrv),
and WebX68k (HostFS), HTTP fetching with httpclient.x, iperf in both
client and server modes, and SNTP time fetching with sntp.x (UDP) have
been verified. On real hardware, both an X68000Z and an X68000Z XVI
"X EMULATER" combined with x68kzremotedrv (a remote drive service
using a Raspberry Pi Pico W), HTTP fetching with httpclient.x
(including name resolution) and SNTP time fetching with sntp.x have
also been verified. With the current driver, which implements the extended
TI functions, a full telnet.x session (connect, echo exchange,
disconnect, and return to the prompt) has also been verified on all
three environments. telnet.x beta3 has likewise been verified in the
same way. nc.x (netcat-x68k) has likewise been verified on all three
environments, over both TCP (client and listening) and UDP.
WebXpressionPlus fetching and displaying plain
HTTP pages (including following a link to a second page) has likewise
been verified on all three environments.

## Download

A prebuilt package (a zip containing `hfsnet.x` and `hfsnethost.py`) is
available from [Releases](https://github.com/renatus-novus-x/hfsnet/releases).

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
| 10 | 2 | Auxiliary argument (socket type for OPEN (0 = SOCK_STREAM, 1 = SOCK_DGRAM), LISTEN backlog, ACCEPT destination slot, SHUTDOWN how) |
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
psocket / sockerr / sockstate / sock_top / ntoa_sock / gethostbyname

The pointer-returning lookup functions (gethostbyaddr /
getnetbyname / getnetbyaddr / getservbyname / getservbyport /
getprotobyname / getprotobynumber) return NULL, not -1, when the
lookup fails. Any other unimplemented function returns -1. Limitations:

- Socket types supported are TCP (SOCK_STREAM) and UDP (SOCK_DGRAM);
  socket() returns -1 for RAW
- ICMP is not supported. ping.x opens a raw socket (SOCK_RAW) because it
  uses ICMP (Internet Control Message Protocol), but this driver supports
  TCP / UDP only and returns -1 from socket() for raw sockets. TCP and
  UDP alone still cover a reasonable range of uses
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

The host side needs no build.

## Usage

1. Create a folder for sharing (e.g. `hfsnet-share` on the PC) and connect
   it as a guest drive through the emulator's HFS or similar
   (connect with write permission)
2. Start the host-side companion:
   `python3 host/hfsnethost.py <path to the shared folder>`
3. Load the driver on the guest (give the folder's guest-side path):
   `hfsnet.x C:\HFSNET`
4. Applications that use libsocket (such as the elf2x68k sample
   httpclient.x) can then communicate as-is

The driver can also be removed without rebooting. Give `/r` instead
of the folder (`-r` works too):

`hfsnet.x /r`

- On success it prints `hfsnet: removed.`
- If the driver is not resident it prints `hfsnet: not installed.`
  (this is not an error)
- If any socket is still in use it refuses, printing
  `hfsnet: cannot remove: N socket(s) in use.`
- Do not remove the driver while a network application is running
  (an application may have cached the driver's entry point)

## Known limitations

- Latency: every TI call becomes a file round trip, so applications that
  repeat small reads and writes will be slow. RECV waits up to 300 ms on
  the host side before answering "no data", which keeps useless round
  trips down
- Resident memory: the driver keeps its whole image resident
- Loading the driver twice is detected via the existing TCP/IP driver
  search and stopped
