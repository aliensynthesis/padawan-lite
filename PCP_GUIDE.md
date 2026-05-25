# PCP — PAD Control Protocol User Guide

For Padawan-Lite v1.2 (`bridge/pcp.{h,c}`).

## Overview

### What it is

The **PAD Control Protocol (PCP)** is a side-channel TCP listener
that Padawan-Lite exposes so a host application can drive the PAD's
X.29 message layer over a *second* TCP connection, separate from
the user's data session.

PCP is a line-oriented ASCII protocol. Each command and response
is a single CRLF-terminated line. There are eight host-to-bridge
commands and three response classes (`OK`, `ERR`, `EVT`). Every
command after `BIND` maps 1:1 onto an X.29 PAD-message function
defined in ITU-T X.29 clause 4.5: Set, Read, Set-and-Read,
Parameter Indication, Invitation to Clear, Indication of Break,
and Error.

Enable PCP with `--pcp-port <port>` when launching `padawan-lite`.
The listener binds to `127.0.0.1` only.

### Why it's needed

#### What X.29 was for, historically

When public packet-switched data networks like Telenet, Tymnet,
Datapac, and Transpac became the way to reach remote hosts from
character terminals, the PAD sat in the carrier's equipment
between the user and the host. Three different parties touched
the session, and each had different needs:

- The **user's terminal** drove the PAD's local behaviour
  (echoing, line editing, break key) via the X.28 DTE/PAD
  interface.
- The **PAD itself** held a set of behavioural parameters
  defined by X.3 (parameters 1–22 of the basic set: echo,
  forwarding characters, padding, page wait, break action, ...)
  controlling how it presented the user's keystrokes to the host
  and the host's output to the user.
- The **remote host application** ran on the far side of the
  X.25 virtual call. The PAD was not under its control — yet the
  application needed to influence PAD behaviour at key moments
  in the dialogue.

The third party had no in-band way to reach the PAD. The data
channel was reserved for user-to-host traffic. ITU-T X.29
(1980, revised through 1997) closed that gap by defining a
host-to-PAD message format carried in X.25 data packets with
the **Q-bit** set. A Q=1 packet on a virtual call is
"qualified" data — addressed to the PAD itself rather than
forwarded onward to the user — so the host could speak to the
PAD without disturbing the user's view of the connection.

Concrete historical use cases for X.29:

- **Password masking.** Before prompting for a password, the
  host issued `SET 2:0` (echo off) so the user's keystrokes
  weren't reflected on the terminal; afterwards, `SET 2:1`
  restored echo. Without X.29 the host would have had to print
  enough leading characters to mask the typed password — a
  hack used on raw telephone modems but never necessary on a
  PSPDN.
- **Mode switching.** A line-mode editor that suddenly needed
  character-at-a-time input (full-screen programs, vi-style
  editors) issued `SET 3:0` (no forwarding character) plus
  `SET 4:20` (1-second idle timer) so each keystroke reached the
  host promptly. Exiting the editor restored the prior mode.
- **File transfer.** Before pushing a binary file to the
  terminal, a host turned off echo, forwarding, padding, and
  page-wait so PAD-level processing didn't corrupt the stream;
  a `Set-and-Read` returned the previous values for the host to
  restore.
- **Adapting to the user's PAD.** A host used `Read` to inspect
  the user's profile and adapt its output (line lengths, page
  pause, character set) rather than assume a fixed terminal type.
- **Graceful clear.** When an application exited, the host sent
  Invitation to Clear so the PAD itself originated the X.25
  clear (giving the user a clean PAD prompt) instead of
  yanking the virtual call out from under them.
- **Break handling.** When the user pressed BREAK, the PAD
  forwarded an Indication of Break to the host so the host
  could discard buffered output and respond — and conversely the
  host could ask the PAD to convey a break-style interruption to
  the user.
- **Error indication.** When the host detected a protocol error
  in something the PAD sent (e.g. an unknown X.29 message
  type), it replied with X.29 Error so the PAD could surface a
  diagnostic.

These were not optional polish. Any non-trivial PSPDN-era
application — timesharing logins, BBS-style services, online
databases, packet-network email frontends — depended on X.29 to
present a usable experience to a remote terminal user.

#### Why a side-channel is needed today

Padawan-Lite reaches the host over **Telnet/TCP**, not X.25, and
Telnet has no Q-bit. Without some workaround, the host cannot
ask the PAD to change parameters, read parameters, or signal a
break — the X.29 dispatcher in `src/pad.c` sits idle, and every
use case above goes unmet.

The canonical workaround — wrapping X.29 messages inside a
custom Telnet IAC SB option — requires every host to patch its
Telnet library. For retro hosts, embedded systems, and any host
whose Telnet stack is not under the operator's control, that is
impractical.

PCP avoids the problem entirely:

- The host opens a **second** TCP connection to the bridge,
  alongside its existing Telnet data connection.
- The host issues `BIND` to attach this control connection to the
  data connection's PAD session (matched by the bridge's local
  IP:port for that session).
- From then on, the host issues plain ASCII commands (`SET 2:0`,
  `READ 1,2,3`, `ICLR`, ...). The bridge encodes each command
  into the corresponding X.29 message and feeds it to the PAD as
  if it had arrived on a `Q=1` packet.
- X.29 messages the PAD sends back (e.g. a Parameter Indication
  in response to `READ`) are decoded by the bridge and delivered
  to the host as `EVT` lines on the same control connection.

The result: a host can drive the full X.29 PAD-message surface
over Telnet without modifying its Telnet library.

### How it fits the bridge

```
                            +---------------------+
                            |    host program     |
                            +----------+----------+
                                 |     |
                Telnet (data)    |     |   PCP (control, text)
                                 |     |
                            +----v-----v----+
                            |   bridge      |
                            |               |
                            |  pad_session  |
                            |  (X.28 core)  |
                            +-------+-------+
                                    |
                                    |  X.28 in-band
                                    |
                            +-------v-------+
                            |     user      |
                            |  (terminal)   |
                            +---------------+
```

The data connection carries the user-to-host traffic exactly as
before. The PCP connection is parallel and carries only X.29
control traffic. The PAD core (`src/pad.c`, `libpadawancore.a`)
is unchanged — PCP plugs into the existing X.25 service interface
and routes Q=1 packets through itself.

### Security model

- The listener binds to `127.0.0.1` only.
- On `BIND`, the bridge checks that the source IP of the PCP
  connection equals the peer IP of the data connection it is
  trying to attach to. A control connection from a different host
  cannot bind another host's data session, even on a multi-user
  bridge.
- At most one PCP connection may be bound to a given session at
  a time. A second `BIND` to the same session returns
  `ERR session-already-bound`.
- There is no authentication or encryption on the PCP listener
  itself. Treat it like a local debugging port: do not widen the
  bind address without adding your own gating (e.g. an SSH tunnel
  or a reverse proxy with auth).

---

## Programmer's reference

### Connection lifecycle

1. **Connect.** Open a TCP connection to `127.0.0.1:<pcp-port>`.
   On accept, the bridge sends a single banner line:
   ```
   OK pcp ready
   ```
2. **Bind.** Issue a `BIND` command naming the bridge-local
   endpoint of the data connection you want to control (see
   "Identifying a session" below). On success the bridge replies
   `OK bound`. Subsequent commands are routed to the bound
   session.
3. **Issue commands.** Once bound, any of the X.29 commands
   below may be issued. Each is acknowledged synchronously with
   `OK` (the X.29 message was fed to the PAD) or `ERR <reason>`
   (the command was rejected before the PAD saw anything).
4. **Receive events.** X.29 messages the PAD sends back arrive
   asynchronously as `EVT` lines on the same connection — they
   are interleaved with command/response traffic.
5. **Close.** Send `QUIT` (the bridge replies `OK bye` and closes
   the socket), or simply close the TCP connection. If the bound
   PAD session is torn down by other means (user disconnects, the
   call is cleared, ...), the control connection remains open
   but unbound — subsequent commands return `ERR not-bound` until
   the host issues a fresh `BIND`.

### Wire format

- Every line is terminated by `\r\n` outbound and may be
  terminated by `\n` or `\r\n` inbound. Trailing whitespace and
  CR are stripped before parsing.
- Lines beginning with `#` and empty lines are ignored — useful
  for scripted/replay testing.
- Maximum line length is 256 bytes. A line that overflows the
  buffer without an LF causes the bridge to discard the buffer
  and send `ERR line-too-long`.
- Keywords (`BIND`, `SET`, `READ`, ...) are case-insensitive.
  Decimal numeric arguments are required where shown.
- Up to **16** PCP control connections may be active at once
  (`PCP_MAX_CONNS`). A 17th `accept` is responded to with
  `ERR pcp-busy\r\n` and the socket is closed.

### Identifying a session

`BIND <ip>:<port>` names the **bridge-local** address of the
*outbound* Telnet connection associated with the data session.
This is the address the bridge picked when it called the host's
remote service on the user's behalf — the same `(local_ip,
local_port)` the host's `accept(2)` returns for the data
connection.

A typical host program flow:

```c
int data_fd = accept(listen_sock, &client_addr, &len);

struct sockaddr_in local;
socklen_t llen = sizeof(local);
getsockname(data_fd, (struct sockaddr *)&local, &llen);

char bridge_ip[INET_ADDRSTRLEN];
inet_ntop(AF_INET, &local.sin_addr, bridge_ip, sizeof(bridge_ip));
int  bridge_port = ntohs(local.sin_port);

int pcp_fd = connect_to("127.0.0.1", pcp_port);
read_banner(pcp_fd);                       /* "OK pcp ready" */
dprintf(pcp_fd, "BIND %s:%d\r\n", bridge_ip, bridge_port);
```

### Commands (host → bridge)

| Command | Syntax | X.29 message |
|---------|--------|--------------|
| `BIND`     | `BIND <ip>:<port>` | (none — control plane) |
| `SET`      | `SET <ref>:<val>[,<ref>:<val>...]` | Set (§4.5.1) |
| `READ`     | `READ <ref>[,<ref>...]` | Read (§4.5.2) |
| `SETREAD`  | `SETREAD <ref>:<val>[,<ref>:<val>...]` | Set-and-Read (§4.5.1/2) |
| `PAR`      | `PAR <ref>:<val>[,<ref>:<val>...]` | Parameter Indication (§4.5.4) |
| `ICLR`     | `ICLR` | Invitation to Clear (§4.5.3) |
| `BREAK`    | `BREAK [<param8>]` | Indication of Break (§4.5.5) |
| `ERR`      | `ERR <code>` | Error response (§4.5.6) |
| `QUIT`     | `QUIT` | (none — closes the control conn) |

Field rules:

- **`<ref>`** is an X.3 parameter reference, 1–255 (`READ`
  rejects 0; `SET`/`SETREAD`/`PAR` accept 0–255 but the PAD will
  reject unknown refs).
- **`<val>`** is a decimal byte, 0–255.
- Up to **32** ref/value pairs (`X29_MAX_PAIRS`) per `SET` /
  `READ` / `SETREAD` / `PAR` command.
- `BREAK` accepts an optional X.3 param-8 value (the "break
  action" to convey to the remote PAD); omit the argument for a
  bare break indication.
- `ERR <code>` carries the X.29 error-code byte. The
  diagnostic-explanation field is fixed at `0` in v1.2.
- `BIND <ip>` must be an IPv4 dotted-quad and `<port>` must be
  1–65535.

### Responses (bridge → host)

Each line is one of three classes:

| Class | Format | Meaning |
|-------|--------|---------|
| `OK`   | `OK` or `OK <info>` | Command accepted. For commands that map to an X.29 message, this means the message has been delivered to the PAD; the PAD's reaction (if any) will arrive later as an `EVT`. |
| `ERR`  | `ERR <reason>` | Command rejected; no X.29 message was sent to the PAD. |
| `EVT`  | `EVT <type> [<args>]` | Asynchronous X.29 event from the PAD. May arrive at any time once bound. |

The accept banner is `OK pcp ready`. A successful `BIND` is
`OK bound`. A successful `QUIT` is `OK bye` followed by the
bridge closing the socket. All other successful commands reply
with bare `OK`.

### Error reasons

The strings below are exhaustive — the bridge emits no others.

| Reason | Trigger |
|--------|---------|
| `usage: BIND <ip>:<port>` | `BIND` argument missing the colon. |
| `bad ip` | `BIND` IP field empty or longer than `INET_ADDRSTRLEN`. |
| `bad port` | `BIND` port not in 1–65535. |
| `no-such-session` | No active session has the named bridge-local endpoint. |
| `session-has-no-peer` | Session exists but has no associated remote peer (race during teardown). |
| `source-ip-mismatch` | PCP source IP ≠ data-connection peer IP for the named session. |
| `session-already-bound` | Another PCP connection is already bound to that session. |
| `not-bound` | A command other than `BIND` was issued before a successful `BIND`, or the session was torn down after binding. |
| `syntax` | Argument list rejected by the parser (e.g. malformed `ref:val`, ref out of range, missing value). |
| `encode` | The X.29 encoder rejected the message (e.g. buffer too small — should not happen with current limits). |
| `unknown-command` | First token did not match any keyword. |
| `line-too-long` | Input line exceeded 256 bytes without an LF; receive buffer was discarded. |
| `pcp-busy` | Sent on accept when all 16 connection slots are in use; the socket is then closed. |

### Events (bridge → host)

When a bound PAD session emits an X.29 message back (via
`x25_send(qbit=1)` from inside `src/pad.c`), the bridge decodes
the wire body and writes one `EVT` line to the PCP connection.
Event keywords mirror the command keywords:

| Event | Format | Source X.29 message |
|-------|--------|---------------------|
| `EVT PAR ref:val,...` | mirror of `PAR` command | Parameter Indication (§4.5.4) |
| `EVT ICLR` | bare | Invitation to Clear (§4.5.3) |
| `EVT SET ref:val,...` | mirror of `SET` command | Set (§4.5.1) |
| `EVT SETREAD ref:val,...` | mirror of `SETREAD` command | Set-and-Read (§4.5.1/2) |
| `EVT READ ref,...` | mirror of `READ` command | Read (§4.5.2) |
| `EVT BREAK [param8]` | mirror of `BREAK` command | Indication of Break (§4.5.5) |
| `EVT ERR <code>` | mirror of `ERR` command | Error response (§4.5.6) |
| `EVT UNKNOWN` | bare | Any X.29 message that fails to decode, plus Reselection (§4.5.7) and any future type. |

Reselection (X.29 §4.5.7) is recognised but not yet dispatched
by the PAD core (see `deviations.txt`); a Reselection on the
wire shows up as `EVT UNKNOWN`.

### Worked example

A host program wants to put the bound session into transparent
mode (`SET 2:0, 3:0, 4:20`), then ask the PAD for the current
values of params 2 and 3, and finally invite the user's PAD to
clear the call cleanly.

```
host  >  (TCP connect to 127.0.0.1:<pcp-port>)
bridge<  OK pcp ready

host  >  BIND 10.0.0.5:31420
bridge<  OK bound

host  >  SET 2:0,3:0,4:20
bridge<  OK

host  >  READ 2,3
bridge<  OK
bridge<  EVT PAR 2:0,3:0          # PAD's response to the Read

host  >  ICLR
bridge<  OK

host  >  QUIT
bridge<  OK bye
         (socket closed)
```

The intermediate `EVT PAR` line is the PAD's Parameter Indication
arriving asynchronously after the `READ` was processed. A robust
host should treat the connection as a stream of lines and
dispatch each line on its first token (`OK`, `ERR`, `EVT`), not
assume a strict command/response pairing.

### Limits at a glance

| Limit | Value | Source |
|-------|-------|--------|
| Listener bind address | `127.0.0.1` (fixed) | `bridge/pcp.c::pcp_init` |
| Concurrent PCP connections | 16 | `PCP_MAX_CONNS` |
| Inbound line buffer | 256 bytes | `PCP_LINE_MAX` |
| Per-conn receive buffer | 512 bytes | `PCP_RECV_BUF` |
| Pairs per `SET`/`READ`/`SETREAD`/`PAR` | 32 | `X29_MAX_PAIRS` |
| `ref` range | 0–255 (`READ`: 1–255) | parser |
| `val` range | 0–255 | parser |

### Related references

- `bridge/pcp.h` — public PCP API and protocol header comment.
- `bridge/pcp.c` — implementation.
- `include/x29.h` — X.29 encoder/decoder + `x29_pair_t`,
  `x29_message_t`, `X29_MAX_PAIRS`, message-type enum.
- `src/pad.c` — X.29 dispatcher inside the PAD core.
- `kb/X.29-1997.pdf` — ITU-T X.29 (12/97), authoritative spec
  for the PAD-message wire format.
- `deviations.txt` — known gaps (Reselection not dispatched, the
  diagnostic-explanation byte in `ERR` fixed at 0, etc.).
