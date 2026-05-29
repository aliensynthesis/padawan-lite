# Padawan-Lite Telenet Personality — Quick Reference

For `--emulate telenet` as of Padawan-Lite v1.4.1.

Companion to [`QUICKREF.md`](QUICKREF.md). That doc covers Padawan-Lite
in general; this one is the reference card for what the Telenet
personality specifically changes.

## Invocation

```
padawan-lite --emulate telenet [other flags...]
```

The `telenet` personality is opt-in. Without `--emulate`, the PAD
uses the `default` personality (X.28 standard). Everything in
[`QUICKREF.md`](QUICKREF.md) still applies; this document lists
*only* the Telenet-specific surface.

## Handshake

| Step | Behaviour |
| --- | --- |
| First user `<CR>` | Silent — autobaud heritage; session holds in `DTE_WAITING`. |
| Second user `<CR>` | Emits the `TELENET` banner, the address line, and the `TERMINAL=` prompt. |
| User types terminal type + `<CR>` (or just `<CR>`) | Stored on session (informational only — no X.3 effect); emit `@` prompt. |
| Session ready | At `@` prompt in `PAD_WAITING`. |

Banner format (two lines + prompt):

```
TELENET
<bridge IP:port>
TERMINAL=
```

The `<bridge IP:port>` is the listen-side address the user reached
via `getsockname` on the accepted TCP connection. Empty in stdin
sessions; the address line is suppressed.

## Prompt and recall

| Item | Value |
| --- | --- |
| PAD prompt character | `@` |
| PAD recall character (X.3 #1) | `DLE` / Ctrl-P (inherited from simple-profile default) |
| Recall mode | Multi-shot — issue commands repeatedly; only `CONT`/`CONTINUE` returns to data mode |
| Prompt re-emitted after each recall command | Yes (param 6 bit 2 forced on) |
| Two-CR handshake | Yes (`handshake_acks_needed = 2`) |

Note: Telenet user-doc text `CR @ CR` describes the user-side
*escape* pattern of clients like Prime's NETLINK (where `@` was
the configurable local escape character), NOT a PAD-level recall.
The PAD recall really was DLE. See [`deviations.txt`](deviations.txt)
[2026-05-25 amended 2026-05-29].

## Command aliases (in addition to standard X.28 verbs)

| Alias(es) | Dispatches as | Equivalent to |
| --- | --- | --- |
| `C`, `CONNECT` | `X28_CMD_SELECTION` (with address) | `CALL <addr>` |
| `D`, `DISCONNECT` | `X28_CMD_CLR` | `CLR` |
| `CONT`, `CONTINUE` | `X28_CMD_CONTINUE` (return to data mode) | (no X.28 equivalent) |
| `HALF` | `X28_CMD_SET` with preset `2:0` | `SET 2:0` (echo off, half-duplex) |
| `FULL` | `X28_CMD_SET` with preset `2:1` | `SET 2:1` (echo on, full-duplex) |

All aliases require whitespace (or end-of-input) after the keyword
— `c 12345` works; `c12345` does not match the alias and falls
through to the bare-SELECTION parser (which produces an empty
address).

`CONT`/`CONTINUE` is load-bearing under Telenet: PAD recall is
multi-shot, so this is the only way back to data mode (under the
default personality the post-dispatch auto-return covers this; under
Telenet it does not).

## Service signal text

The connected and disconnected signals are emitted as
`<called_address> TEXT` per Telenet convention; the `<address>` is
captured at SELECTION dispatch and remains set through the
connect/clear lifecycle.

| Signal | Telenet text | Prefix? |
| --- | --- | --- |
| Connected (COM, §3.5.21) | `CONNECTED` | yes |
| Free (STAT, §3.5.11) | `READY` | n/a (no call) |
| Engaged (STAT, §3.5.11) | `BUSY` | n/a (different context) |
| Error (ERR, §3.5.19) | `?` | n/a |
| Clear-confirmation (§3.5.17) | `DISCONNECTED` | yes |

NUI prompt (bare `ID` command, §5.2 paragraph): `ID?` instead of
the default `NUI?`.

## Clear-cause text (per `pad_clear_cause_t` index)

Eight causes have authoritative Telenet user-doc text; six fall
through to the X.28-standard short abbreviation; the `DTE`-cause
text doubles as the clear-confirmation override.

| Idx | Cause | Telenet text | Prefix? | Source |
| --- | --- | --- | --- | --- |
| 0 | `OCC` Number busy | `BUSY` | yes | user-doc |
| 1 | `NC` Network congestion | (X.28 default: `NC`) | — | — |
| 2 | `INV` Invalid facility | (X.28 default: `INV`) | — | — |
| 3 | `NA` Access barred | `ILLEGAL DESTINATION ADDRESS` | **NO** | user-doc |
| 4 | `ERR` Local procedure error | (X.28 default: `ERR`) | — | — |
| 5 | `RPE` Remote procedure error | (X.28 default: `RPE`) | — | — |
| 6 | `NP` Number not assigned | `ILLEGAL ADDRESS` | **NO** | user-doc |
| 7 | `OOO` Out of order | `NOT OPERATING` | yes | user-doc |
| 8 | `DTE` Cleared by remote DTE | `DISCONNECTED` | yes | user-doc |
| 9 | `DER` Remote device error | `NOT RESPONDING` | yes | user-doc |
| 10 | `RCH` Reverse charging refused | `REFUSED COLLECT CONNECTION` | yes | user-doc |
| 11 | `ID` Incompatible destination | `DOES NOT SUPPORT TERMINAL` | yes | user-doc |
| 12 | `SHN` Ship not contacted | (X.28 default: `SHN`) | — | — |
| 13 | `FNA` Fast select refused | (X.28 default: `FNA`) | — | — |
| 14 | `RNA` Cannot route | `NOT REACHABLE` | yes | user-doc |

Per-cause prefix suppression is controlled by
`personality_t.clr_text_skip_address_prefix` — a 16-bit mask whose
bit N skips the `<address>` prefix for cause N. Telenet sets
`(1<<3) | (1<<6)` because the `NA` and `NP` messages are about the
address being unusable, so prefixing them would be confusing.

Two user-doc messages with no clean X.28 mapping fall through to
X.28-standard text rather than being squeezed into the wrong cause:

| User-doc text | Description | Why not mapped |
| --- | --- | --- |
| `ILLEGAL SOURCE ADDRESS` | "Refusing because of terminal's address" | Closest fit is `RPE`, but conflating it with the general-purpose `RPE` cause would overload one slot with two unrelated meanings |
| `<address> NOT AVAILABLE` | "Operating but actively refusing" | No clean `pad_clear_cause_t` slot; same reasoning |

## X.3 parameter overlay

Values that Telenet sets differently from the simple-profile
default. Entries marked `KEEP` inherit the simple-profile value
without an override.

| # | Name | Telenet value | Source | Note |
| --- | --- | --- | --- | --- |
| 1 | recall | `KEEP` (= 1 = DLE) | user-doc, NETLINK | T30 abort char in NETLINK is also `0x10` |
| 2 | echo | `1` | NETLINK | |
| 3 | forward | `2` (CR only) | **deviation** | NETLINK says `126` (every condition); deferred — see §"Known deviations" below |
| 4 | idle timer | `KEEP` (= 0) | NETLINK | |
| 5 | device flow | `1` (XON/XOFF) | NETLINK | |
| 6 | signals | `5` (standard + prompt bit) | **deviation** | NETLINK says `1` (no prompt bit); requires decoupling prompt-emit from X.3 — deferred |
| 7 | break action | `21` (discard + indicate + interrupt) | NETLINK | v1.3.1 |
| 11 | speed | `KEEP` | NETLINK says `3` (1200) but read-only | |
| 12 | DTE flow | `1` (XON/XOFF) | NETLINK | |
| 13 | lf_insert | `4` (LF echo to DTE) | NETLINK | v1.3.1 |
| 15 | edit | `KEEP` (= 0) | NETLINK | Editing engaged via param 19 instead |
| 16 | cdel | `127` (DEL) | NETLINK | v1.3.1; previously inherited `8` (BS) |
| 17 | ldel | `KEEP` (= 24, CAN) | NETLINK match | |
| 18 | ldis | `KEEP` (= 18, DC2) | NETLINK match | |
| 19 | esig | `1` (editing signals on) | NETLINK | v1.3.1 |
| 20–22 | various | `KEEP` (= 0) | NETLINK match | |

The Telenet extended X.3 namespace (T13, T17, T18, T30, T36, T40,
T45 — signalled by the `(0, 33)` national-options marker in
NETLINK's parameter stream) is **not modelled**. Padawan-Lite's
X.3 only handles the basic 1–22 + extended 23–30 namespace
defined by ITU-T.

## Telenet personality flags (summary)

| `personality_t` field | Value for Telenet |
| --- | --- |
| `name` | `"telenet"` |
| `banner` | `"TELENET"` |
| `prompt_char` | `'@'` |
| `nui_prompt` | `"ID?"` |
| `connected_text` | `"CONNECTED"` |
| `free_text` | `"READY"` |
| `engaged_text` | `"BUSY"` |
| `err_text` | `"?"` |
| `clr_confirm_text` | `"DISCONNECTED"` |
| `clr_text[]` | see table above |
| `profile_overlay` | `TELENET_PROFILE_OVERLAY` (see above) |
| `command_aliases` | `TELENET_ALIASES` (see above) |
| `emit_address` | `1` (emit address line after banner) |
| `handshake_acks_needed` | `2` (two-CR handshake) |
| `prefix_called_address_on_call_signals` | `1` |
| `clr_text_skip_address_prefix` | `(1<<3) \| (1<<6)` (NA, NP) |
| `keep_command_mode_after_recall` | `1` (multi-shot) |
| `terminal_type_prompt` | `"TERMINAL="` |

## Sourcing pyramid

Every Telenet value lives at one of four tiers; the higher the
tier, the more authoritative.

| Tier | Source | Examples |
| --- | --- | --- |
| 1 | **ITU-T X.28 / X.3** — definitive | All `KEEP`-marked X.3 params; clear-cause indices; service-signal taxonomy |
| 2 | **Prime NETLINK source** (X.25SRC>NETLINK>X3_INIT.INS.PL1, c.1988) | X.3 overlay values (params 2, 5, 6, 7, 12, 13, 16, 19); v1.3.1 |
| 3 | **Telenet user documentation** (supplied by project owner) | Command aliases; clear-cause text strings; banner-then-address layout; `@` prompt convention; two-CR handshake; multi-shot recall + `CONTINUE`; `TERMINAL=` prompt; v1.4.1 |
| 4 | **Best-effort / VERIFY** (still flagged in source) | Exact banner format (just `TELENET`, no node ID); some edge cases not enumerated in available references |

NETLINK source does **not** carry clear-cause text strings — those
live in an external data file (`PRIMENET*>NETLINK>CLEARING_CAUSES`)
not part of the X.25SRC distribution. v1.4.1's cause text is at
tier 3 (Telenet user-doc), neither verified nor contradicted by
NETLINK. See [`deviations.txt`](deviations.txt) [2026-05-29].

## Known deviations from authentic Telenet

These are documented compromises, not bugs. Each is fixable with
known design work; deferred deliberately. See
[`deviations.txt`](deviations.txt) for full rationale per item.

| Item | Telenet (authoritative) | Padawan-Lite | Why deferred |
| --- | --- | --- | --- |
| X.3 #3 forwarding mask | `126` (every condition) | `2` (CR only) | Significant UX change; users can `SET 3:126` per session |
| X.3 #6 service signals | `1` (no prompt bit) | `5` (prompt bit on) | Requires decoupling prompt emission from X.3 param 6 bit 2 |
| Telenet extended X.3 (T13, T17, T18, T30, T36, T40, T45) | Defined in NETLINK | Not modelled | Would need a Telenet-specific X.3 extension namespace; v1.x scope limit |
| `ILLEGAL SOURCE ADDRESS` clear-text | Telenet emits this | Falls through to X.28-default `RPE` | No clean X.28 cause; overloading `RPE` rejected |
| `<address> NOT AVAILABLE` clear-text | Telenet emits this | Falls through | Same reasoning as above |
| Terminal-type → X.3 profile mapping | Telenet had a TTP table (PT45, VT100…) | Captured into `pad_session_t.terminal_type` informational-only | Would need authoritative TTP-→-profile mapping data |
| Authentic banner with node ID | `TELENET 215 5D`-style | Just `TELENET` | Node ID is per-deployment; bridge user can override via `pad_set_identification` |

## See also

- [`README.md`](README.md) — top-level project overview; mentions
  Telenet in the Features section.
- [`QUICKREF.md`](QUICKREF.md) — the general Padawan-Lite quick
  reference (X.28 standard commands, X.3 parameter set, flag
  summary).
- [`deviations.txt`](deviations.txt) — dated narrative of every
  deviation and sourcing decision. The Telenet entries are at
  `[2026-05-24]`, `[2026-05-25 amended 2026-05-29]`, multiple
  `[2026-05-26]` entries, and three `[2026-05-29]` entries.
- [`CHANGELOG.md`](CHANGELOG.md) — release-by-release additions.
  Telenet personality first shipped in v1.2.0; substantive
  refinement in v1.3.0, v1.3.1, and v1.4.1.
- [`PCP_GUIDE.md`](PCP_GUIDE.md) — sibling guide (PAD Control
  Protocol) modelling the prose-heavy guide style. This document
  is its terse cousin.
- `src/personality.c` — `PERSONALITY_TELENET`, `TELENET_ALIASES`,
  `TELENET_CLR_TEXT`, `TELENET_PROFILE_OVERLAY`, all with
  per-row source citations.
