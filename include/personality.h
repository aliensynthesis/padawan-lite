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
