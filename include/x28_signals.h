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

/* ITU-T X.28 (12/97) - PAD command signal parsing and PAD service signal
   formatting (X.28 clause 3.5).

   Scope for v1.2: the local control commands (PAR?, PROF, SET, SET?, CLR,
   ICLR, STAT, RESET, INT) and the remote variants (RPAR?, RSET). A SELECTION
   command captures the raw text of a call-setup signal; deeper parsing of
   facility blocks (X.28 3.5.15) is deferred. */
#ifndef PADAWAN_X28_SIGNALS_H
#define PADAWAN_X28_SIGNALS_H

#include "types.h"

#define X28_MAX_PARAMS         22  /* in scope: refs 1..22 */
#define X28_MAX_ADDRESS_LEN    128 /* selection-signal address capture */
#define X28_MAX_FACILITIES     8   /* facility codes per selection signal */
#define X28_MAX_CUD            128 /* selection-signal call user data */
#define X28_FACILITY_ARG_MAX   31  /* per-facility argument length */

typedef enum {
    X28_CMD_UNKNOWN = 0,
    X28_CMD_PAR,        /* PAR?  read local params */
    X28_CMD_RPAR,       /* RPAR? read remote params */
    X28_CMD_PROF,       /* PROF <id> */
    X28_CMD_SET,        /* SET p:v[,p:v...] */
    X28_CMD_SET_READ,   /* SET? p:v[,p:v...] */
    X28_CMD_RSET,       /* RSET p:v[,p:v...] (remote set and read) */
    X28_CMD_CLR,        /* CLR */
    X28_CMD_ICLR,       /* ICLR (invitation to clear remote PAD) */
    X28_CMD_STAT,       /* STAT */
    X28_CMD_RESET,      /* RESET */
    X28_CMD_INT,        /* INT */
    X28_CMD_SELECTION,  /* call setup (digits + optional facility/user data) */
    /* X.28 clause 5: extended dialogue mode commands. */
    X28_CMD_BREAK,      /* BREAK - acts as break signal (no escape, §5.1) */
    X28_CMD_NUI_ON,     /* ID <NUI string>  (§5.2) */
    X28_CMD_NUI_OFF,    /* IDOFF (§5.2) */
    X28_CMD_LANG,       /* LANG / LANGUAGE <language string> (§5.3) */
    X28_CMD_HELP,       /* HELP [<subject>] (§5.4) */
    /* Padawan-Lite extension: explicit no-op command used by
       personalities (e.g. Telenet's "CONTINUE" / "CONT") that need a
       named keyword to return the user from PAD command state back to
       data-transfer state. The state-machine transition at the end of
       feed_command_byte already returns to DATA_TRANSFER when a call
       is up, so the dispatcher just consumes this command silently.
       Not exposed by the X.28-standard parser; reachable only via a
       personality command-alias table. */
    X28_CMD_CONTINUE
} x28_cmd_type_t;

typedef struct {
    uint8 ref;
    uint8 value;
    uint8 invalid; /* X.28 3.5.14: when set, format as "INV" in PAR output */
} x28_param_pair_t;

/* One parsed facility request from a selection PAD command signal
   (X.28 §3.5.15.1 + Table 4/X.28). The code is an IA5 letter (e.g.
   'N' = NUI, 'R' = reverse charging, 'G' = CUG); the arg holds any
   subsequent characters up to the next ',' separator (NUL-terminated). */
typedef struct x28_facility_t {
    uint8 code;
    uint8 arg_len;
    char  arg[X28_FACILITY_ARG_MAX + 1];
} x28_facility_t;

typedef struct {
    x28_cmd_type_t   type;
    uint8            param_count;
    x28_param_pair_t params[X28_MAX_PARAMS];
    uint8            profile_id;
    uint8            address_len;
    char             address[X28_MAX_ADDRESS_LEN + 1]; /* NUL-terminated */
    /* For SELECTION commands (also reached via the CALL keyword): the
       facility block from §3.5.15.1 if present. Empty when the selection
       signal has no facility block (just an address). */
    uint8            facility_count;
    x28_facility_t   facilities[X28_MAX_FACILITIES];
    /* SELECTION call user data field (§3.5.15.3). cud_type is the prefix
       letter ('D' = IA5 string, 'P' = printable IA5 subset, 'H' = hex
       pairs representing binary) or 0 when no CUD was supplied. cud_data
       holds the bytes after the prefix (with any leading whitespace
       stripped), NUL-terminated; for 'H' fields the hex pairs are kept
       verbatim and are not decoded. */
    uint8            cud_type;
    uint8            cud_len;
    char             cud_data[X28_MAX_CUD + 1];
} x28_command_t;

/* Parser return codes. */
#define X28_PARSE_OK             0
#define X28_PARSE_ERR_EMPTY      1
#define X28_PARSE_ERR_SYNTAX     2
#define X28_PARSE_ERR_BAD_REF    3
#define X28_PARSE_ERR_BAD_VALUE  4
#define X28_PARSE_ERR_OVERFLOW   5

/* Optional per-personality command-keyword alias. Lets a personality
   add command synonyms (e.g. Telenet's "C"/"CONNECT" for CALL,
   "D"/"DISCONNECT" for CLR) without modifying the X.28 command set.

   - keyword is uppercase ASCII; matching is case-insensitive.
   - cmd_type is the X28_CMD_* enum value the alias dispatches to.
   - takes_address: 1 means a selection-style address argument follows
     the keyword (and is fed to the same parser as CALL's address);
     0 means the alias is a bare command with no argument.
   - preset_pairs: optional pointer to a flat array of ref/value bytes
     (laid out ref0, val0, ref1, val1, ...). When non-NULL the parser
     pre-populates out->params[] with these pairs before returning.
     Useful for named-SET aliases (e.g. Telenet's "HALF" = SET 2:0).
     NULL means no preset args. Only meaningful for X28_CMD_SET
     dispatch; ignored for other cmd_type values.
   - preset_pair_count: number of (ref,value) pairs in preset_pairs
     (so the preset_pairs array holds 2 * preset_pair_count bytes).
     Bounded by X28_MAX_PARAMS.

   Matching follows the same terminator rule as the built-in keywords:
   the byte after the keyword (if any) must be non-alphanumeric, so
   e.g. "CONNECT 12345" matches but "CONNECT12345" does not. Use a
   space (or tab) to separate the keyword from its address argument.

   Alias matching is checked AFTER the built-in keywords and BEFORE
   the SELECTION fallthrough, so a personality may add new synonyms
   but cannot override the X.28-standard command names. The array is
   terminated by an entry with keyword == NULL. */
typedef struct {
    const char  *keyword;
    uint8        cmd_type;
    uint8        takes_address;
    const uint8 *preset_pairs;
    uint8        preset_pair_count;
} x28_command_alias_t;

/* Parse a PAD command signal (X.28 clause 3.5). Trailing CR (0/13) or
   PLUS (2/11) delimiter is accepted but not required. Returns X28_PARSE_OK
   or one of the error codes.

   aliases is an optional NULL-terminated array of personality-supplied
   command synonyms (see x28_command_alias_t); pass NULL when no
   personality is active. */
int x28_parse_command(const char *input, uint32 len,
                      const x28_command_alias_t *aliases,
                      x28_command_t *out);

/* PAD service signal formatters (X.28 clause 3.5).
   Each writes into buf and returns the number of bytes written, or a
   negative value if buf_size is insufficient. */

/* Format effector alone, used as the acknowledgement PAD service signal
   (X.28 3.5.3): CR LF. Padding chars per param 9 are added by the upper
   layer when emitting on the line. */
int32 x28_format_ack(uint8 *buf, uint32 buf_size);

/* Parameter value PAD service signal (X.28 3.5.14):
       PAR p1:v1, p2:v2, ... <CR><LF> */
int32 x28_format_par(const x28_param_pair_t *params, uint8 count,
                     uint8 *buf, uint32 buf_size);

/* Error PAD service signal (X.28 3.5.19): "ERR" <CR><LF>. */
int32 x28_format_err(uint8 *buf, uint32 buf_size);

/* Clear indication PAD service signal (X.28 3.5.17). cause_text is one of
   the strings from Table 6/X.28 (e.g. "OCC" for number busy, "DTE" for
   remote request); cause_code/diagnostic are decimal numbers per X.25.
   Format: "CLR <cause_text> C:<code> D:<diag>" <CR><LF>. */
int32 x28_format_clr_indication(const char *cause_text,
                                uint8 cause_code, uint8 diagnostic,
                                uint8 *buf, uint32 buf_size);

/* Clear confirmation PAD service signal (X.28 3.5.9 + Table 7/X.28):
       "CLR CONF" <CR><LF> */
int32 x28_format_clr_confirmation(uint8 *buf, uint32 buf_size);

/* Reset indication PAD service signal (X.28 3.5.7):
       "RESET <cause> D:<diag>" <CR><LF> */
int32 x28_format_reset_indication(const char *cause_text, uint8 diagnostic,
                                  uint8 *buf, uint32 buf_size);

/* Status responses (X.28 3.5.11). */
int32 x28_format_status_engaged(uint8 *buf, uint32 buf_size);
int32 x28_format_status_free(uint8 *buf, uint32 buf_size);

/* Connected PAD service signal (X.28 §3.5.21):
       <effector> "COM" <effector>
   v1.2 emits the minimal form without the optional called-address,
   facility, or call-user-data blocks (deferred until X.25). */
int32 x28_format_connected(uint8 *buf, uint32 buf_size);

/* Help PAD service signal (X.28 3.5.5 / §5.5). The subject string is from
   the HELP command and selects the response text per Table 10/X.28; v1.2
   emits a single canned description regardless of subject. */
int32 x28_format_help(const char *subject, uint8 *buf, uint32 buf_size);

#endif
