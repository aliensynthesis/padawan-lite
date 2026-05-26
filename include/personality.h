/*
** Copyright 2026 Alien Synthesis
**
** Redistribution and use in source and binary forms, with or without
** modification, are permitted provided that the following conditions
** are met:
**
**   1. Redistributions of source code must retain the above copyright
**      notice, this list of conditions and the following disclaimer.
**
**   2. Redistributions in binary form must reproduce the above copyright
**      notice, this list of conditions and the following disclaimer in
**      the documentation and/or other materials provided with the
**      distribution.
**
** THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER "AS IS" AND ANY
** EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
** IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
** PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER BE
** LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
** CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
** SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
** BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
** WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
** OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE,
** EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* PAD personality system.

   The PAD's user-facing surface (banner text, prompt character, NUI
   prompt, service-signal abbreviations) is defined by ITU-T X.28 only
   in the abstract; the actual strings on the wire varied by network
   operator. A real Telenet PAD said "@" where X.28 specifies "*";
   said "BUSY" where X.28 specifies "OCC"; identified itself with a
   network-specific banner. The personality system lets the same
   underlying PAD state machine present a different visible
   personality at runtime.

   Each personality_t carries strings (and a small profile-overlay
   table) that override the X.28-standard defaults. NULL entries in
   the personality fall through to the X.28 default formatting, so
   the "default" personality is just an all-NULL table — useful as
   a baseline and as the result when no --emulate flag is given.

   See src/personality.c for the built-in tables (default, telenet,
   tymnet) and the lookup function. */
#ifndef PADAWAN_PERSONALITY_H
#define PADAWAN_PERSONALITY_H

#include "types.h"
#include "x28_signals.h"
#include "x3.h"

/* Number of pad_clear_cause_t enum values currently defined (kept in
   sync with the enum in pad.h). The personality's per-cause text
   table is sized to this. */
#define PERSONALITY_CLR_CAUSE_COUNT 15

typedef struct personality {
    /* Lookup key for --emulate. ASCII, lowercase, no spaces. */
    const char *name;

    /* Replaces the default PAD identification text emitted at
       handshake (X.28 §3.5.18). NULL = use whatever
       pad_set_identification supplied. Empty string = suppress the
       PAD-ID emission entirely. */
    const char *banner;

    /* Single byte emitted as the PAD-ready prompt (X.28 §3.5 plus
       the §3.1.3 "prompt" semantic). X.28-standard implementations
       use '*' (2/10); some networks use other characters. 0 here
       means "use the X.28 default" ('*'). */
    uint8 prompt_char;

    /* Prompt text issued when the user types bare ID (X.28 §5.2,
       Padawan-Lite extension that prompts for the NUI). NULL = use
       the default "NUI?". */
    const char *nui_prompt;

    /* Override for the X.28 "COM" connected service signal (§3.5.21).
       NULL = use "COM". */
    const char *connected_text;

    /* Overrides for X.28 STAT replies (§3.5.11): "FREE" / "ENGAGED".
       NULL = use defaults. */
    const char *free_text;
    const char *engaged_text;

    /* Override for the X.28 "ERR" error signal (§3.5.19). NULL = use
       "ERR". */
    const char *err_text;

    /* Override for the X.28 clear-confirmation service signal
       ("CLR CONF" in standard X.28, §3.5.17). Emitted when the user
       clears the call (e.g. via the CLR command). NULL = use the
       X.28-standard formatter. When prefix_called_address_on_call_signals
       is also set, the called address is prepended (Telenet style:
       "<address> DISCONNECTED"). */
    const char *clr_confirm_text;

    /* Per-cause override text for the X.28 clear-indication service
       signal (§3.5.17 + Table 6/X.28). Indexed by pad_clear_cause_t.
       NULL entry = use the X.28-standard abbreviation. */
    const char *clr_text[PERSONALITY_CLR_CAUSE_COUNT];

    /* X.3 parameter overlay applied at session start, AFTER the
       profile selected via PROF is loaded. Index N == X.3 param N.
       NULL pointer = no overlay. Index 0 is unused; an entry of 0xFF
       in another slot means "leave the profile default in place."
       This lets a personality enforce its own preferred PAD settings
       (e.g. Tymnet's typical CR-only forwarding) without requiring
       the user to also pass --telnet-defaults. */
    const uint8 *profile_overlay;

    /* Optional NULL-terminated array of command-keyword aliases the
       PAD command parser will recognise in addition to the X.28
       built-ins. Lets a personality add network-specific synonyms
       (Telenet's "C"/"CONNECT" for CALL, "D"/"DISCONNECT" for CLR)
       without changing the underlying X.28 command set. NULL = the
       personality adds no aliases. See x28_command_alias_t in
       include/x28_signals.h for matching rules. */
    const x28_command_alias_t *command_aliases;

    /* When non-zero, the PAD identification handshake emits a second
       line carrying the synthetic PAD network address (pad_address,
       set by the bridge via pad_set_address). Mirrors how a real
       Telenet PAD identified itself by its own area-and-node address
       on the line after the "TELENET" banner. Off by default; the
       second line is suppressed when this flag is 0 OR when
       pad_address is empty. */
    uint8 emit_address;

    /* Number of carriage-returns the user must send to complete the
       initial X.28 §2.2 handshake. 0 or 1 = single-step handshake
       (the first CR drives ACTIVE_LINK -> ... -> PAD_WAITING in one
       go, with banner + prompt emitted at the same time -- our
       default behaviour). 2 = the historical PSPDN two-CR pattern:
       the first CR holds in DTE_WAITING (autobaud-equivalent
       acknowledgement, silent on the wire), the second CR finishes
       the transition and emits the banner + prompt. Telenet's user
       documentation specifies the two-CR form; default and Tymnet
       keep the single-CR behaviour. */
    uint8 handshake_acks_needed;

    /* When non-zero, the personality-supplied connected/clear-confirm/
       clear-indication signals are emitted in the form
       "<called_address> <text>" rather than just "<text>".
       Matches Telenet's documented "<address> CONNECTED" /
       "<address> DISCONNECTED" / "<address> BUSY" pattern. The
       prefix is suppressed when called_address is empty (e.g. for
       signals emitted before any call has been placed). Has no
       effect on signals that don't carry personality-supplied text
       (those still use the X.28-standard formatter). */
    uint8 prefix_called_address_on_call_signals;

    /* When non-zero, PAD recall is multi-shot: after the user types
       a command in WAITING_FOR_CMD with a connected call, the
       session stays in command mode and emits a fresh prompt
       instead of auto-returning to DATA_TRANSFER. The user issues
       an explicit CONTINUE / CONT to go back to the call. Matches
       Telenet's documented behaviour. When 0 (default and Tymnet),
       the X.28 §3.2.1.5 one-shot recall behaviour applies: any
       command in WAITING_FOR_CMD auto-returns to DATA_TRANSFER. */
    uint8 keep_command_mode_after_recall;

    /* Optional terminal-type prompt emitted at handshake completion
       after the banner (and the address line, if any). When NULL,
       no prompt is emitted and complete_handshake proceeds straight
       to PAD_WAITING + @ prompt -- the default X.28 behaviour. When
       non-NULL, the literal text (e.g. "TERMINAL=") is emitted with
       no surrounding format effectors, the session enters
       PAD_STATE_AWAITING_TERMINAL_TYPE, and free-form input is
       captured (echoed) into pad_session_t.terminal_type until CR.
       The captured value is recorded but not currently used to
       configure any X.3 parameters; a future enhancement could map
       known terminal-type IDs to X.3 profiles. */
    const char *terminal_type_prompt;
} personality_t;

/* "leave alone" sentinel for profile_overlay entries. Personality
   designers use this for params where they don't want to deviate
   from the loaded profile. */
#define PERSONALITY_KEEP 0xFF

/* Return the built-in personality with the given name, or NULL if
   none matches. The "default" personality is returned by both
   name = NULL and name = "default". */
const personality_t *personality_by_name(const char *name);

/* Apply a personality's profile_overlay (if any) to a freshly-loaded
   X.3 parameter set. Skips PERSONALITY_KEEP entries and the
   read-only X3_PAR_SPEED. */
void personality_apply_profile_overlay(const personality_t *pers,
                                       x3_params_t *params);

#endif
