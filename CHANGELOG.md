# Changelog

All notable changes to Padawan-Lite are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/) and the project
adheres to [Semantic Versioning](https://semver.org/).

## [1.0.0] — 2026-05-23

Initial public release of **Padawan-Lite**, the lightweight C89
component of the broader JEDI framework: a portable ITU-T X.28
PAD-to-Telnet bridge that lets character-mode clients reach modern
Telnet hosts with the look-and-feel of a real packet-network PAD.

### Spec compliance

- **ITU-T X.28** — DTE/PAD interface: state machine, command parser,
  PAD service signals, profile loading, recall, break, editing, page
  wait, flow control.
- **ITU-T X.3** — full basic parameter set (refs 1–22, behavioural) and
  extended parameter set (refs 23–30, storage + range-validated;
  behaviour inert pending future use).
- **ITU-T X.29** — PAD-message wire format and dispatcher: Set, Read,
  Set-and-Read, Parameter Indication, Invitation to Clear, Indication
  of Break, Error. Reselection is decoded but deferred.
- 594 unit tests citing spec clauses across `tests/test_pad`,
  `tests/test_x28_signals`, `tests/test_x29_messages`, and
  `tests/test_x3`.
- Every known deviation from the standards is recorded in
  [`deviations.txt`](deviations.txt).

### Features

- **Two run modes** — single-session stdin/stdout (interactive demo
  with raw-tty intercepts for Ctrl-P recall, Ctrl-B break, Ctrl-D
  quit) and multi-session TCP server (`--listen PORT`, up to 16
  concurrent users).
- **Address map** — `--config FILE` resolves X.25-style addresses to
  `<host>:<port>` pairs; unmapped addresses fall back to
  `localhost:<address-as-port>`.
- **NUI authentication** — `--auth FILE` gates calls against an
  allow-list of network user identifiers. Honours both the per-call
  `N` facility (X.28 §3.5.15.1.1) and the session-level NUI set via
  the `ID` command (X.28 §5.2). Bare `ID` prompts for the NUI string.
- **Authentic baud-rate throttling** — `--baud BPS` paces I/O in both
  directions with TCP backpressure so no bytes are lost. Suitable
  300 / 1200 / 2400 baud feel for retro client compatibility.
- **Telnet-friendly defaults** — `--telnet-defaults` overrides X.3
  params 2 / 3 / 4 to clean up interaction with stream-oriented
  Telnet hosts.
- **Inbound traffic logging** — `--trace` writes per-session
  `hexdump -C` logs of bytes the PAD receives from the client and
  from the host. `--trace-prefix PREFIX` overrides the default
  filename prefix. `--trace-line-mode` consolidates client input
  by CR for easier reading when service-side character echo is
  in play.
- **Selection signal facility block** — parsed into structured
  `x28_facility_t` array; recognised codes include N (NUI),
  R (reverse charging), G (CUG), F (fast select), Q (charging),
  T (transit delay).
- **Call user data field** (X.28 §3.5.15.3) — `D` / `P` / `H` prefix
  recognised and captured.
- **HELP service signal** — subject-specific responses per Table 10
  of X.28 for all implemented commands.

### Architecture

- **Pluggable transport** — Padawan-Lite reaches the network through
  an abstract X.25 service interface (`include/x25.h`); the Telnet/TCP
  bridge is one implementation. The interface carries the Q-bit so a
  future X.25 transport can convey X.29 PAD messages natively.
- **Shared core library** — `libpadawancore.a` contains the PAD state
  machine, X.3 parameter store, X.28 signal codec, and X.29 message
  codec. Build via `make lib`. Designed for reuse by future framework
  components (the bare-name "Padawan" with an EX.25 / gRPC back-end,
  and the X.25↔Telnet gateway project) so the spec implementation
  lives in exactly one place.
- **Extraction-ready bridge** — `bridge/` depends only on Padawan-
  Lite's public headers and can be lifted into a separate project
  without code changes.

### Build

- Linux x86_64 with GCC 11+ and GNU make.
- Strict C89: `-std=c89 -Wall -Wpedantic -Wextra -Werror`.
- No external dependencies beyond a POSIX libc.

### Known limitations

- **No real X.25 transport.** The Telnet bridge is the only X.25
  implementation in v1.0; it can't carry Q-bit data, so the X.29
  encode/decode and dispatcher are dormant when paired with the
  bridge. They are exercised by the platform stub for tests and
  will go live when a real X.25 backend or the planned PCP
  side-channel ships.
- **Reselection PAD message** (X.29 §4.5.7) is decoded but not
  dispatched.
- **Annex C non-English service signal text** is not implemented;
  responses are English-only.
- **Multi-aspect PAD (X.28 clause 6 / X.8)** is out of scope.
- Smaller deviations enumerated in [`deviations.txt`](deviations.txt).

[1.0.0]: https://example.invalid/padawan-lite/releases/tag/v1.0.0
