# Changelog

All notable changes to Padawan-Lite are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/) and the project
adheres to [Semantic Versioning](https://semver.org/).

## [1.4.0] — 2026-05-29

### Removed

- **Tymnet personality removed.** `--emulate tymnet` is no longer
  recognised; `personality_by_name("tymnet")` now returns NULL.

  Rationale: Tymnet's actual implementation — in both its
  Tymshare-era and later McDonnell Douglas / BT North America
  incarnations — proved to be too technically disjoint from the
  X.28 / X.29 conventions that the personality system is shaped
  around. Tymnet was effectively a separate end-to-end
  asynchronous protocol family with its own network layer, its
  own framing, its own session-establishment dance, and a
  command surface that did not map cleanly onto X.3 parameter
  overlays + X.28 service-signal text overrides. Producing a
  user-visible Tymnet personality accurate enough to justify
  the name would have required morphing the existing PAD core
  well beyond the "X.28-with-overrides" model. Keeping a
  placeholder Tymnet personality whose data tables were
  best-effort reconstruction (as the v1.2 VERIFY notes flagged)
  perpetuated a misleading impression that Padawan-Lite spoke
  Tymnet.

  Affected files: `src/personality.c` (removed `TYMNET_*` tables
  and registry entry), `include/personality.h`, `include/pad.h`,
  `bridge/main.c` (removed from documentation and the
  `--emulate` usage list), `tests/test_personality.c` (removed
  four Tymnet-specific tests; updated `test_lookup_known` to
  assert `tymnet` returns NULL as a regression guard),
  `README.md`, `QUICKREF.md`. `PCP_GUIDE.md` retains a
  historical mention of Tymnet as one of the real PSPDNs —
  that's factual context, not a feature claim.

  See `deviations.txt` [2026-05-29] for the full rationale and
  the criteria for ever revisiting.

### Test count

- 768 tests pass (was 774; six Tymnet-specific tests removed).

### Note on versioning

This is a breaking change for any consumer scripting
`--emulate tymnet`. By strict semver this would warrant a
2.0.0 bump; we keep the 1.x line because Tymnet's inclusion
in v1.2 was always flagged as best-effort and the removal
restores the personality system to its intended scope
(X.28-shaped PADs only).

[1.4.0]: https://example.invalid/padawan-lite/releases/tag/v1.4.0

## [1.3.1] — 2026-05-29

### Changed

- **Telenet personality X.3 overlay now sourced from Prime
  Computer's NETLINK X.25 client** (X.25SRC>NETLINK>X3_INIT.INS.PL1,
  c.1988) rather than best-effort reconstruction. NETLINK was the
  production user-side X.25 client for PRIMOS and had to
  interoperate with real Telenet service, so its initialiser block
  is the closest available primary-source reference outside of
  GTE's operator manuals.

  Four `TELENET_PROFILE_OVERLAY` values updated:
  - **param 7** (break action): `2` → `21` (discard output + send
    indication of break + interrupt; was "reset only")
  - **param 13** (lf_insert): inherit-from-profile → `4` (LF echo
    to DTE)
  - **param 16** (cdel / char delete): inherit-from-profile (= `8`
    BS) → `127` (DEL). Real Telenet expected the DEL key for
    character delete, not Backspace.
  - **param 19** (esig): inherit-from-profile → `1` (editing PAD
    service signals enabled)

  Three further mismatches between our overlay and NETLINK's
  initialiser block were identified but deliberately NOT applied in
  this release because each carries larger design implications:
  - param 3 (forward) — NETLINK = 126, we still use 2 (CR only)
  - param 6 (signals) — NETLINK = 1, we still use 5; needs
    decoupling of prompt-emission logic from the X.3 param
  - Telenet extended X.3 namespace (T13, T17, T18, T30, T36) — we
    don't model the extension namespace at all

  All deferred items documented in `deviations.txt`.

- **Recall character documentation amended.** The earlier
  deviations.txt entry [2026-05-25] inferred from Telenet user-doc
  "CR @ CR" pattern that authentic Telenet recall was `@` (X.3
  param 1 = 64) and that our choice of DLE / Ctrl-P was a
  modern-convenience deviation. NETLINK source confirms Telenet
  PADs actually used param 1 = 1 (DLE) and a separate "abort
  character" Telenet extension T30 = 0x10 (also DLE). Our choice
  was correct all along; the "CR @ CR" sequence in Telenet docs
  was the *user-side* escape pattern in clients like NETLINK,
  where '@' was the configurable local escape character, NOT the
  PAD-level recall. Deviation flag removed; no code change.

### Verified

- 774 tests still pass after the overlay updates — existing tests
  verify behaviours, not exact X.3 default values.

[1.3.1]: https://example.invalid/padawan-lite/releases/tag/v1.3.1

## [1.3.0] — 2026-05-26

### Added

- **Personality command-alias mechanism.** New parser hook
  (`x28_command_alias_t` in `include/x28_signals.h`) lets a
  personality publish network-specific command keywords beyond
  the X.28 standard set, optionally with preset SET pairs for
  "named-SET" aliases. Matched after standard X.28 keywords and
  before the SELECTION fallthrough -- personalities add synonyms
  but never override standard names. Wired through
  `personality_t.command_aliases` and `pad.c::feed_command_byte`.
- **Telenet personality refinements based on Telenet user-doc
  research.** Several behaviours added to make `--emulate telenet`
  faithful to documented Telenet user experience:
  - Command synonyms: `C` / `CONNECT` (CALL), `D` / `DISCONNECT`
    (CLR), `CONT` / `CONTINUE` (return-to-data after recall),
    `HALF` (SET 2:0, echo off), `FULL` (SET 2:1, echo on).
  - **Two-CR handshake** (`personality_t.handshake_acks_needed`):
    Telenet's autobaud convention -- the first CR holds in
    DTE_WAITING, the second emits the banner and proceeds.
  - **Address-line identity after banner**
    (`personality_t.emit_address` + `pad_set_address`): mirrors
    Telenet's "TELENET\n<address>" display by emitting the bridge's
    local listen IP:port on the line after the banner. Bridge
    captures via `getsockname` per TCP accept.
  - **Address-prefixed call signals**
    (`personality_t.prefix_called_address_on_call_signals`):
    `CONNECTED` / `DISCONNECTED` / cause-text indications are
    rendered as `<address> CONNECTED` / `<address> DISCONNECTED`
    instead of the standard X.28 forms. New
    `personality_t.clr_confirm_text` field for the
    clear-confirmation override.
  - **Multi-shot PAD recall**
    (`personality_t.keep_command_mode_after_recall`): Telenet
    lets users issue several commands per recall, with explicit
    `CONT` / `CONTINUE` to return to data mode. The `CONTINUE`
    handler is now load-bearing under this mode.
  - **`TERMINAL=` terminal-type prompt**
    (`personality_t.terminal_type_prompt`, new PAD state
    `PAD_STATE_AWAITING_TERMINAL_TYPE`): emitted after the
    banner+address; user response captured free-form (until CR)
    into `pad_session_t.terminal_type`. Informational only; no
    X.3 profile mapping currently applied.
- New `X28_CMD_CONTINUE` enum value (Padawan-Lite extension;
  reachable only via personality aliases).
- New API `pad_set_address(p, text)` for the bridge to communicate
  its local listen IP:port to the PAD.
- 119 new unit tests (was 619 at v1.2; total now 774) across
  `tests/test_personality.c` and `tests/test_x28_signals.c`,
  covering the alias mechanism, preset-pair dispatch, every Telenet
  alias, the two-CR handshake, the address-line emission, the
  address-prefix signals, multi-shot recall, the `TERMINAL=`
  capture path, and the X.28 default-personality regressions for
  each of them.

### Fixed

- **PAD recall now emits the `@` prompt.** X.28 §3.5.23 requires
  the prompt when the PAD is ready for a command. Previously,
  recall from `DATA_TRANSFER` or `CONN_IN_PROGRESS` silently
  changed state without emitting the prompt -- users had to type
  blind. Universal fix; gated only by X.3 param 6's prompt bit.
- **`CLR` command now emits the `@` prompt** after the
  clear-confirmation. Previously the dispatch set state directly
  to `PAD_WAITING` and bypassed the post-dispatch prompt logic,
  leaving the user without a visible prompt until they typed
  another command.

### Changed

- The Telenet personality's profile overlay still leaves X.3
  param 1 (PAD recall character) at the simple-profile default
  (DLE / Ctrl-P). Authentic Telenet used `@` (decimal 64), but
  `@` is too prevalent in modern user input (email addresses,
  vi commands, shell paths) to commandeer as PAD recall.
  Documented in `deviations.txt`; users wanting authentic
  recall can issue `SET 1:64` per session.

[1.3.0]: https://example.invalid/padawan-lite/releases/tag/v1.3.0

## [1.2.0] — 2026-05-24

### Added

- **PAD personality system.** `--emulate <name>` selects a personality
  that overrides the visible PAD surface (banner, prompt character,
  NUI prompt, service-signal text, X.3 profile overlay) without
  changing the X.28 state machine. The session looks and feels like
  the named historical PSPDN PAD while the underlying core remains
  spec-compliant. Built-in personalities:
  - `default` — X.28-standard behaviour (the v1.1 surface);
    selected automatically when `--emulate` is omitted.
  - `telenet` — GTE Telenet style: `@` prompt, `ID?` NUI prompt,
    `READY` / `BUSY` / `CONNECTED` for STAT/ENGAGED/COM,
    Telenet-style clear-cause text. **VERIFY: data tables are a
    best-effort reconstruction; not validated against primary-source
    GTE Telenet operator manuals. See `src/personality.c` for the
    explicit caveats.**
  - `tymnet` — Tymnet style: lowercase status text, "please log in:"
    banner, "user name:" NUI prompt, verbose clear messages. **Same
    VERIFY caveat as telenet.**
- New module `src/personality.c` + `include/personality.h` defining
  the `personality_t` struct and `personality_by_name()` lookup.
- New public API `pad_set_personality()` in `include/pad.h`.
- 25 new unit tests in `tests/test_personality.c` cover personality
  lookup, default passthrough, signal-text overrides, profile-overlay
  application, prompt-character visibility through the handshake
  path, banner override, and NULL-reverts-to-default.

### Changed

- `pad_session_t` gains a `personality` pointer (NULL = default
  X.28 behaviour).
- `pad.c::emit_prompt_if_enabled`, `emit_err`, `emit_status`,
  `emit_connected`, the bare-ID NUI-prompt path, and
  `pad_remote_cleared` consult the personality before falling back
  to the X.28-standard formatters in `x28_signals.c`.
- `telenet` and `tymnet` profile overlays now set X.3 param 6 to
  `5` (= standard service signals + prompt bit), so the `@`
  (Telenet) / `*` (Tymnet) prompt is visible by default without
  the user having to issue `SET 6:5` first.
- New helper `emit_signal_text()` in `pad.c` for personality-supplied
  service-signal strings.

[1.2.0]: https://example.invalid/padawan-lite/releases/tag/v1.2.0

## [1.1.0] — 2026-05-23

### Added

- **PAD Control Protocol (PCP).** `--pcp-port PORT` opens a localhost
  TCP listener implementing a line-oriented text control channel.
  Host applications can drive Padawan-Lite's X.29 PAD-message
  functions (`SET`, `READ`, `SETREAD`, `PAR`, `ICLR`, `BREAK`, `ERR`)
  over a second TCP connection without modifying their Telnet
  libraries. Bridge-originated X.29 messages from the PAD are
  translated to `EVT` lines on the same channel, so inbound and
  outbound X.29 traffic both flow over PCP without requiring
  Q-bit transport. Implemented in `bridge/pcp.{h,c}`; reuses the
  existing X.29 encode/decode and dispatcher in `src/x29_messages.c`
  + `src/pad.c` with no PAD-core changes.

### Changed

- `x25_send(qbit=1)` now routes through PCP when a control
  connection is bound to the session; falls back to
  `X25_ERR_NOT_SUPPORTED` when nothing is bound (the pre-1.1
  behaviour).
- `bridge/x25_telnet_bridge.h` exposes two new accessors
  (`x25_bridge_session_at_local`,
  `x25_bridge_peer_ip_for_session`) used by PCP for `BIND` resolution
  and source-IP validation.

### Security

- PCP listener binds `127.0.0.1` only; no flag to widen yet.
- `BIND` requires the PCP control-connection source IP to match the
  data-connection peer IP for the named session.
- At most one PCP connection may be bound to a given session
  concurrently.

[1.1.0]: https://example.invalid/padawan-lite/releases/tag/v1.1.0

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
