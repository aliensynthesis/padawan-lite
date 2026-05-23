# Padawan-Lite

*A light-weight and portable X.28 PAD-to-Telnet bridge.*

Padawan-Lite is a software Packet Assembler/Disassembler — the kind
of device that, back when X.25 packet networks ruled the WAN, let an
ordinary character terminal log into a remote host across the
network. Padawan-Lite implements the ITU-T X.28 DTE/PAD interface,
the X.3 parameter set, and X.29 procedures, but reaches the remote
end through Telnet over TCP instead of through an actual X.25 network.
It is useful for connecting modern Telnet hosts to clients that
expect the look-and-feel of a real PAD — most often a vintage terminal
or retro client software that was originally written for the
packet-switched era.

## Features

- **ITU-T X.28 / X.3 / X.29 compliant.** Every command, every
  parameter, every service signal from the standard. 594 unit tests
  cite the spec clauses they exercise; every known deviation is
  recorded in [`deviations.txt`](deviations.txt).
- **Two run modes.** Single session over stdin/stdout for an
  interactive demo, or a multi-session TCP server (`--listen`,
  max 16 concurrent users) for hosting a real PAD service.
- **NUI authentication.** `--auth FILE` gates calls on
  network-user-identification. Honours both per-call N facilities
  (X.28 §3.5.15.1.1) and session-level NUI set via `ID` (X.28 §5.2).
- **Authentic baud-rate throttling.** `--baud 1200` paces I/O to
  evoke the feel of an analog modem, with TCP backpressure so no
  bytes are lost.
- **Telnet-friendly defaults.** `--telnet-defaults` overrides
  X.3 parameters 2/3/4 for clean interaction with stream-oriented
  Telnet hosts.
- **Inbound traffic logging.** `--trace` writes per-session
  `hexdump -C` logs of bytes the PAD receives from the client and
  from the host; `--trace-line-mode` consolidates client input by CR
  for easy reading when service-side character echo is in play.
- **PAD Control Protocol (PCP).** `--pcp-port PORT` opens a
  localhost text-protocol listener that lets host applications drive
  the X.29 PAD-message layer (SET / READ / SETREAD / PAR / ICLR /
  BREAK / ERR) over a second TCP connection — no Telnet-library
  changes required on the host side.
- **Pluggable transport.** Padawan-Lite talks to the network through
  an abstract X.25 service interface ([`include/x25.h`](include/x25.h));
  the Telnet/TCP bridge is one implementation. Write another if you
  want to reach a real X.25 stack or the planned EX.25 (gRPC) backend.

## Build

Linux with GCC 11+ and GNU make:

```sh
make            # builds padawan-lite binary, libpadawancore.a, and test binaries
make test       # runs the 594-test suite (4 binaries: test_pad, test_x28_signals, test_x29_messages, test_x3)
./padawan-lite -h  # CLI usage summary
```

The build is strict C89: `-std=c89 -Wall -Wpedantic -Wextra -Werror`.
No external dependencies beyond a POSIX libc.

## Quick start

Interactive stdin session against a service listening on local port
30001:

```sh
$ ./padawan-lite
Padawan-Lite v1.1 - profile 1 (simple).
Address = TCP port on localhost (override via -c map).
Press Enter to begin. Ctrl-B = break, Ctrl-P = recall, Ctrl-D = exit.

<Enter>
PADAWAN-LITE v1.1
30001                                 # place the call
COM                                   # connected
... interactive session ...
```

Multi-user TCP listener with NUI auth and a 1200-baud feel:

```sh
$ ./padawan-lite --listen 30000 \
            --auth nuis.txt \
            --baud 1200 \
            --telnet-defaults
Padawan-Lite v1.1 - listening on TCP port 30000 (MAX_SESSIONS = 16).
NUI auth: 2 entries loaded; calls require a matching N facility.
Telnet-friendly defaults: SET 2:0, 3:0, 4:1 applied per session.
Throttle: 120 bytes/sec per direction per session.
```

Users then `telnet host 30000`, complete the X.28 handshake, and
place calls like `Ndavid-30001<CR>` (per-call NUI) or `ID david<CR>`
followed by `30001<CR>` (session-level NUI).

## Command-line flags

| Flag                          | Effect                                                            |
| ----------------------------- | ----------------------------------------------------------------- |
| `-c`, `--config FILE`         | Load address map (`<addr> <host> <port>` per line)                |
| `-l`, `--listen PORT`         | Accept TCP user connections; multi-session                        |
| `-a`, `--auth FILE`           | Require NUI; one allowed NUI per line                             |
| `-t`, `--telnet-defaults`     | Apply `SET 2:0, 3:0, 4:1` per session                             |
| `-b`, `--baud BPS`            | Throttle I/O to BPS bits/sec; `0` = off                           |
| `--trace`                     | Log inbound traffic per session (`hexdump -C` format)             |
| `--trace-prefix PREFIX`       | Override `--trace` filename prefix (implies `--trace`)            |
| `--trace-line-mode`          | Consolidate CLIENT entries by CR (implies `--trace`)              |
| `-h`, `--help`                | Show usage                                                        |

See [`QUICKREF.md`](QUICKREF.md) for the full reference card —
every X.28 command, every X.3 parameter, every service-signal cause
code, plus selection-signal syntax (facility blocks and call user
data) and configuration file formats.

## Configuration files

**Address map** (passed via `-c`): one entry per line, whitespace-
separated.

```
# <address> <host> <port>
30001 localhost 30001
12345 vax.example.com 23
```

Unmapped addresses fall back to `localhost:<address-as-port>`, so
`./padawan-lite` works without a map file as long as your destinations
listen on ports matching the addresses you call.

**Auth file** (passed via `--auth`): one NUI per line.

```
# Authorised NUIs for this PAD
alice
bob
operator1
```

## Tracing

`--trace` writes one log file per session named
`<prefix>-<unix_seconds>-<seq>.log` (default prefix
`padawan-lite-trace`). Each direction transition emits a new
canonical `hexdump -C` entry with a timestamp + direction header:

```
# 2026-05-22 11:23:25.222 SERVICE (64 bytes)
00000000  0a 0d 0a 43 6f 6e 6e 65  63 74 65 64 20 74 6f 20  |...Connected to |
00000010  74 68 65 20 4d 69 63 72  6f 56 41 58 20 33 39 30  |the MicroVAX 390|
...
```

`--trace-line-mode` switches CLIENT-side accumulation to CR-delimited
lines, pairing each input line with the corresponding SERVICE prompt
+ echoes. Entry headers gain a `[LINE MODE]` marker so the
presentation difference is obvious at a glance.

## Project layout

```
include/                   public headers (pad.h, x3.h, x28_signals.h, x25.h, types.h)
src/                       platform-independent implementation
platform/<name>/           per-platform shims (linux today; structure ready for others)
bridge/                    Telnet/TCP X.25 bridge + interactive driver (produces ./padawan-lite)
tests/                     one test_<topic>.c per module, no external framework
kb/                        authoritative ITU-T spec PDFs (X.1, X.2, X.3, X.8, X.25, X.28, X.29)
deviations.txt             every known deviation / out-of-scope item
QUICKREF.md                user-facing command and parameter cheat sheet
```

The Telnet bridge is decoupled from the PAD core through
`include/x25.h`; the bridge directory is extraction-ready for a
separate X.25↔Telnet gateway project planned alongside Padawan-Lite.

## Standards and conformance

Padawan-Lite implements the following ITU-T recommendations (PDFs in
[`kb/`](kb/)):

- **X.3** — PAD parameters (refs 1–22 of the basic set)
- **X.28** — DTE/PAD interface (commands, service signals, state machine)
- **X.29** — PAD procedures over X.25 (partial; remote-PAD RPAR/RSET only)
- **X.25** — interface and protocol (out of scope; replaced by the Telnet bridge)
- **X.1**, **X.2**, **X.8** — supporting specifications

## Testing

The test suite has no external dependencies: each
`tests/test_<topic>.c` defines its own `ASSERT_*` macros and runs the
relevant cases. Every assertion embeds the reference input/output
pairs from the specification with the expected value and clause
number stated in a comment. New work should land with a test that
cites the clause it exercises.

```sh
$ make test
==> tests/test_pad
ok 252/252
==> tests/test_x28_signals
ok 199/199
==> tests/test_x29_messages
ok 73/73
==> tests/test_x3
ok 70/70
```

## Contributing

- Every deviation from the spec — including obvious omissions and
  intentional out-of-scope items — must be appended to
  [`deviations.txt`](deviations.txt), not only commented in source.
- Source citations belong in code comments
  (e.g. `/* X.28 §3.5.17 */`); narrative goes in
  [`deviations.txt`](deviations.txt) or [`QUICKREF.md`](QUICKREF.md).
- Don't add `//` comments, C99+ features, or compiler extensions
  outside platform guards.
- Don't introduce dependencies into the PAD core or the bridge; the
  binary should keep building on a stock Linux toolchain.
- Don't widen the `include/x25.h` interface without good reason — it
  is the seam between Padawan-Lite and any future X.25 layer or alternate
  transport, and keeping it small lets the bridge directory remain
  extractable.

