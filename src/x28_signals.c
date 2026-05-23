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

/* X.28 clause 3.5: PAD command signal parser and PAD service signal
   formatters.

   Per X.28 3.5: command signal characters are from IA5 columns 2-7,
   case-insensitive; SP (2/0) and DEL (7/15) are ignored where not assigned
   to editing; terminator is CR (0/13) or PLUS (2/11). */
#include "x28_signals.h"
#include "x3.h"
#include <string.h>

/* IA5 character constants referenced by clause 3.5 (column/row notation). */
#define IA5_CR    0x0D  /* 0/13  PAD command signal delimiter */
#define IA5_LF    0x0A  /* 0/10  in format effector */
#define IA5_PLUS  0x2B  /* 2/11  alternate PAD command signal delimiter */
#define IA5_SP    0x20  /* 2/0   ignored in command signals */
#define IA5_DEL   0x7F  /* 7/15  ignored in command signals */
#define IA5_COMMA 0x2C  /* 2/12  parameter separator */
#define IA5_COLON 0x3A  /* 3/10  ref/value separator */
#define IA5_QMARK 0x3F  /* 3/15  read indicator */

/* --- small character helpers -------------------------------------------- */

static int to_upper(uint8 c)
{
    if (c >= 'a' && c <= 'z') {
        return (int)c - 'a' + 'A';
    }
    return (int)c;
}

static int is_digit(uint8 c)
{
    return c >= '0' && c <= '9';
}

static int is_letter_or_digit(uint8 c)
{
    int u = to_upper(c);
    if (u >= 'A' && u <= 'Z') return 1;
    return is_digit(c);
}

static uint32 skip_ignored(const char *s, uint32 i, uint32 len)
{
    /* §3.5: SP and DEL are not part of the command (unless assigned to an
       editing function; v1.0 does not assign them, so we always skip). */
    while (i < len && ((uint8)s[i] == IA5_SP || (uint8)s[i] == IA5_DEL)) {
        i++;
    }
    return i;
}

static int parse_u8(const char *s, uint32 i, uint32 len,
                    uint8 *out, uint32 *next)
{
    uint32 v = 0;
    uint32 start = i;
    while (i < len && is_digit((uint8)s[i])) {
        v = v * 10 + (uint32)(s[i] - '0');
        if (v > 255) return 0;
        i++;
    }
    if (i == start) return 0;
    *out = (uint8)v;
    *next = i;
    return 1;
}

/* Match keyword kw[0..klen-1] against s[i..] case-insensitively. Returns 1
   only if the next char (if any) is not a letter or digit, so "SET" matches
   "SET", "SET ", "SET?" but not "SETUP". */
static int match_kw(const char *s, uint32 i, uint32 len,
                    const char *kw, uint32 klen)
{
    uint32 j;
    if (i + klen > len) return 0;
    for (j = 0; j < klen; j++) {
        if (to_upper((uint8)s[i + j]) != kw[j]) return 0;
    }
    if (i + klen == len) return 1;
    if (is_letter_or_digit((uint8)s[i + klen])) return 0;
    return 1;
}

/* --- argument-list parsers ---------------------------------------------- */

/* Parse a comma-separated list of parameter references into out->params[].
   Used by PAR? and RPAR?. An empty list is allowed and is treated by the
   spec as "all parameters" (caller decides). */
static int parse_ref_list(const char *s, uint32 i, uint32 len,
                          x28_command_t *out)
{
    uint8 ref;

    i = skip_ignored(s, i, len);
    if (i >= len) {
        return X28_PARSE_OK; /* empty list = all */
    }

    for (;;) {
        i = skip_ignored(s, i, len);
        if (!parse_u8(s, i, len, &ref, &i)) {
            return X28_PARSE_ERR_SYNTAX;
        }
        if (ref < X3_PAR_MIN || ref > X3_PAR_MAX) {
            return X28_PARSE_ERR_BAD_REF;
        }
        if (out->param_count >= X28_MAX_PARAMS) {
            return X28_PARSE_ERR_OVERFLOW;
        }
        out->params[out->param_count].ref = ref;
        out->params[out->param_count].value = 0;
        out->param_count++;

        i = skip_ignored(s, i, len);
        if (i >= len) return X28_PARSE_OK;
        if ((uint8)s[i] != IA5_COMMA) return X28_PARSE_ERR_SYNTAX;
        i++;
    }
}

/* Parse a comma-separated list of ref:value pairs. Used by SET, SET?, RSET. */
static int parse_pair_list(const char *s, uint32 i, uint32 len,
                           x28_command_t *out)
{
    uint8 ref;
    uint8 value;

    i = skip_ignored(s, i, len);
    if (i >= len) {
        return X28_PARSE_ERR_SYNTAX; /* SET requires at least one pair */
    }

    for (;;) {
        i = skip_ignored(s, i, len);
        if (!parse_u8(s, i, len, &ref, &i)) return X28_PARSE_ERR_SYNTAX;
        if (ref < X3_PAR_MIN || ref > X3_PAR_MAX) return X28_PARSE_ERR_BAD_REF;

        i = skip_ignored(s, i, len);
        if (i >= len || (uint8)s[i] != IA5_COLON) return X28_PARSE_ERR_SYNTAX;
        i++;
        i = skip_ignored(s, i, len);

        if (!parse_u8(s, i, len, &value, &i)) return X28_PARSE_ERR_SYNTAX;

        /* Parser does not value-validate. X.28 3.3.2 / 3.5.14 require the
           dispatcher to accept valid pairs from a mixed batch and identify
           invalid pairs via "INV" in the PAR response - which means parsing
           must keep the offending pair so dispatch can flag it. */

        if (out->param_count >= X28_MAX_PARAMS) {
            return X28_PARSE_ERR_OVERFLOW;
        }
        out->params[out->param_count].ref = ref;
        out->params[out->param_count].value = value;
        out->params[out->param_count].invalid = 0;
        out->param_count++;

        i = skip_ignored(s, i, len);
        if (i >= len) return X28_PARSE_OK;
        if ((uint8)s[i] != IA5_COMMA) return X28_PARSE_ERR_SYNTAX;
        i++;
    }
}

/* Forward declared: parse_selection uses it for the address remainder. */
static int capture_string_arg(const char *s, uint32 i, uint32 len,
                              x28_command_t *out);

/* True for facility-block letters per Table 4/X.28. Used to decide
   whether a selection PAD command signal begins with a facility
   block or goes straight into the address. */
static int is_facility_letter(uint8 c)
{
    switch (c) {
    case 'B': case 'C': case 'D': case 'E': case 'F': case 'G':
    case 'M': case 'N': case 'O': case 'P': case 'Q': case 'R':
    case 'S': case 'T': case 'U': case 'W':
        return 1;
    default:
        return 0;
    }
}

/* Parse the facility block (between start and end-exclusive) into
   out->facilities[]. Format per §3.5.15.1: facility_code [arg], with
   multiple facilities separated by ','. The arg runs from immediately
   after the letter up to the next ',' or to the block end. */
static int parse_facility_block(const char *s, uint32 start, uint32 end,
                                x28_command_t *out)
{
    uint32 i = start;
    while (i < end) {
        x28_facility_t *f;
        uint8 letter;
        uint32 arg_start;
        uint32 arg_end;

        if (out->facility_count >= X28_MAX_FACILITIES) {
            return X28_PARSE_ERR_OVERFLOW;
        }
        letter = (uint8)s[i++];
        if (!is_facility_letter(letter)) {
            return X28_PARSE_ERR_SYNTAX;
        }
        arg_start = i;
        while (i < end && s[i] != ',') i++;
        arg_end = i;
        if (arg_end - arg_start > X28_FACILITY_ARG_MAX) {
            return X28_PARSE_ERR_OVERFLOW;
        }

        f = &out->facilities[out->facility_count++];
        f->code    = letter;
        f->arg_len = (uint8)(arg_end - arg_start);
        if (f->arg_len > 0) {
            memcpy(f->arg, s + arg_start, f->arg_len);
        }
        f->arg[f->arg_len] = '\0';

        if (i < end && s[i] == ',') i++; /* skip separator */
    }
    return X28_PARSE_OK;
}

/* Parse a selection PAD command signal (§3.5.15). The signal can start
   with a facility block terminated by '-' (per §3.5.15.1), followed by
   the address (§3.5.15.2). Heuristic: if the first character is a
   Table-4 facility letter AND a '-' appears later, treat what's before
   the dash as the facility block; otherwise everything is the address.
   This correctly handles the common case of digit-only addresses that
   happen to contain '-' (X.121 addresses are pure digits, but defensive
   parsers benefit from the heuristic). */
/* Split the address-side input (after any facility block) into the
   address proper and an optional call user data field (§3.5.15.3).
   The CUD is recognised when the first non-address byte is 'D', 'P',
   or 'H' followed by at least one more character; everything from
   that prefix onward is captured as CUD. Pure-digit addresses (X.121)
   never collide with a CUD prefix; addresses containing dots, spaces,
   or tabs are tolerated. If the first non-address byte isn't a CUD
   prefix, the whole input is treated as address (preserving v1.0
   behaviour for non-CUD signals). */
static int capture_address_and_cud(const char *s, uint32 i, uint32 len,
                                   x28_command_t *out)
{
    uint32 rem;
    uint32 j;
    uint32 addr_end;
    uint32 cud_pos = (uint32)-1;
    uint8  cud_type = 0;

    i = skip_ignored(s, i, len);
    rem = len - i;
    if (rem > X28_MAX_ADDRESS_LEN) {
        return X28_PARSE_ERR_OVERFLOW;
    }

    addr_end = rem;
    for (j = 0; j < rem; j++) {
        char c = s[i + j];
        if ((c >= '0' && c <= '9') ||
            c == '.' || c == '-' || c == ' ' || c == '\t') {
            continue;
        }
        if ((c == 'D' || c == 'P' || c == 'H') && j + 1 < rem) {
            cud_pos  = j;
            cud_type = (uint8)c;
        }
        addr_end = j;
        break;
    }

    while (addr_end > 0 &&
           (s[i + addr_end - 1] == ' ' || s[i + addr_end - 1] == '\t')) {
        addr_end--;
    }
    for (j = 0; j < addr_end; j++) {
        out->address[j] = s[i + j];
    }
    out->address[addr_end] = '\0';
    out->address_len = (uint8)addr_end;

    if (cud_pos != (uint32)-1) {
        uint32 cud_start = cud_pos + 1;
        uint32 cud_len;
        while (cud_start < rem &&
               (s[i + cud_start] == ' ' || s[i + cud_start] == '\t')) {
            cud_start++;
        }
        cud_len = rem - cud_start;
        if (cud_len > X28_MAX_CUD) cud_len = X28_MAX_CUD;
        out->cud_type = cud_type;
        for (j = 0; j < cud_len; j++) {
            out->cud_data[j] = s[i + cud_start + j];
        }
        out->cud_data[cud_len] = '\0';
        out->cud_len = (uint8)cud_len;
    }

    return X28_PARSE_OK;
}

static int parse_selection(const char *s, uint32 i, uint32 len,
                           x28_command_t *out)
{
    uint32 dash_pos = (uint32)-1;
    if (i < len && is_facility_letter((uint8)s[i])) {
        uint32 j;
        for (j = i; j < len; j++) {
            if (s[j] == '-') { dash_pos = j; break; }
        }
    }
    if (dash_pos != (uint32)-1) {
        int rc = parse_facility_block(s, i, dash_pos, out);
        if (rc != X28_PARSE_OK) return rc;
        i = dash_pos + 1;
    }
    return capture_address_and_cud(s, i, len, out);
}

/* Capture the rest of the input (after the keyword) into out->address
   as a NUL-terminated string. Used by HELP, LANG, NUI ON, and the
   CALL variant of selection signals. */
static int capture_string_arg(const char *s, uint32 i, uint32 len,
                              x28_command_t *out)
{
    uint32 rem;
    uint32 j;
    i = skip_ignored(s, i, len);
    rem = len - i;
    if (rem > X28_MAX_ADDRESS_LEN) {
        return X28_PARSE_ERR_OVERFLOW;
    }
    for (j = 0; j < rem; j++) {
        out->address[j] = s[i + j];
    }
    out->address[rem] = '\0';
    out->address_len = (uint8)rem;
    return X28_PARSE_OK;
}

/* --- top-level parser --------------------------------------------------- */

int x28_parse_command(const char *input, uint32 len, x28_command_t *out)
{
    uint32 i = 0;

    memset(out, 0, sizeof(*out));
    out->type = X28_CMD_UNKNOWN;

    /* Trim optional trailing CR or PLUS delimiter (§3.5.1). */
    while (len > 0 &&
           ((uint8)input[len - 1] == IA5_CR ||
            (uint8)input[len - 1] == IA5_PLUS)) {
        len--;
    }

    i = skip_ignored(input, i, len);
    if (i >= len) {
        return X28_PARSE_ERR_EMPTY;
    }

    /* Try keywords longest-first so RPAR/RSET beat PAR/SET prefixes. */

    /* RPAR? - remote read */
    if (match_kw(input, i, len, "RPAR", 4)) {
        if (i + 4 >= len || (uint8)input[i + 4] != IA5_QMARK) {
            return X28_PARSE_ERR_SYNTAX;
        }
        out->type = X28_CMD_RPAR;
        return parse_ref_list(input, i + 5, len, out);
    }

    /* RSET p:v[,...] - remote set and read */
    if (match_kw(input, i, len, "RSET", 4)) {
        out->type = X28_CMD_RSET;
        return parse_pair_list(input, i + 4, len, out);
    }

    /* ICLR - invitation to clear (§3.5.8.2) */
    if (match_kw(input, i, len, "ICLR", 4)) {
        out->type = X28_CMD_ICLR;
        return X28_PARSE_OK;
    }

    /* PROF <id> (§3.5.5) */
    if (match_kw(input, i, len, "PROF", 4)) {
        i += 4;
        i = skip_ignored(input, i, len);
        if (i >= len || !parse_u8(input, i, len, &out->profile_id, &i)) {
            return X28_PARSE_ERR_SYNTAX;
        }
        out->type = X28_CMD_PROF;
        return X28_PARSE_OK;
    }

    /* STAT (§3.5.10) */
    if (match_kw(input, i, len, "STAT", 4)) {
        out->type = X28_CMD_STAT;
        return X28_PARSE_OK;
    }

    /* RESET (§3.5.12) */
    if (match_kw(input, i, len, "RESET", 5)) {
        out->type = X28_CMD_RESET;
        return X28_PARSE_OK;
    }

    /* PAR? <refs> (§3.5.4) */
    if (match_kw(input, i, len, "PAR", 3)) {
        if (i + 3 >= len || (uint8)input[i + 3] != IA5_QMARK) {
            return X28_PARSE_ERR_SYNTAX;
        }
        out->type = X28_CMD_PAR;
        return parse_ref_list(input, i + 4, len, out);
    }

    /* SET / SET? (§3.5.6) */
    if (match_kw(input, i, len, "SET", 3)) {
        uint32 next = i + 3;
        if (next < len && (uint8)input[next] == IA5_QMARK) {
            out->type = X28_CMD_SET_READ;
            next++;
        } else {
            out->type = X28_CMD_SET;
        }
        return parse_pair_list(input, next, len, out);
    }

    /* CLR (§3.5.8.1). Optional facility/user-data block is deferred. */
    if (match_kw(input, i, len, "CLR", 3)) {
        out->type = X28_CMD_CLR;
        return X28_PARSE_OK;
    }

    /* INT (§3.5.13) */
    if (match_kw(input, i, len, "INT", 3)) {
        out->type = X28_CMD_INT;
        return X28_PARSE_OK;
    }

    /* X.28 §5 extended dialogue keywords (Table 9/X.28). Accepted regardless
       of param 6's extended-mode bits: the spec says "Some networks may
       provide these keywords when the PAD is not in the extended dialogue
       mode" so accepting them unconditionally is conformant. */

    if (match_kw(input, i, len, "INTERRUPT", 9)) {
        out->type = X28_CMD_INT;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "PARAMETER", 9)) {
        out->type = X28_CMD_PAR;
        return parse_ref_list(input, i + 9, len, out);
    }
    if (match_kw(input, i, len, "LANGUAGE", 8)) {
        out->type = X28_CMD_LANG;
        return capture_string_arg(input, i + 8, len, out);
    }
    if (match_kw(input, i, len, "RSETREAD", 8)) {
        out->type = X28_CMD_RSET;
        return parse_pair_list(input, i + 8, len, out);
    }
    if (match_kw(input, i, len, "SETREAD", 7)) {
        out->type = X28_CMD_SET_READ;
        return parse_pair_list(input, i + 7, len, out);
    }
    if (match_kw(input, i, len, "PROFILE", 7)) {
        uint32 next = i + 7;
        next = skip_ignored(input, next, len);
        if (next >= len ||
            !parse_u8(input, next, len, &out->profile_id, &next)) {
            return X28_PARSE_ERR_SYNTAX;
        }
        out->type = X28_CMD_PROF;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "STATUS", 6)) {
        out->type = X28_CMD_STAT;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "ICLEAR", 6)) {
        out->type = X28_CMD_ICLR;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "RREAD", 5)) {
        out->type = X28_CMD_RPAR;
        return parse_ref_list(input, i + 5, len, out);
    }
    if (match_kw(input, i, len, "CLEAR", 5)) {
        out->type = X28_CMD_CLR;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "BREAK", 5)) {
        out->type = X28_CMD_BREAK;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "IDOFF", 5)) {
        out->type = X28_CMD_NUI_OFF;
        return X28_PARSE_OK;
    }
    if (match_kw(input, i, len, "CALL", 4)) {
        uint32 next = i + 4;
        next = skip_ignored(input, next, len);
        out->type = X28_CMD_SELECTION;
        return parse_selection(input, next, len, out);
    }
    if (match_kw(input, i, len, "READ", 4)) {
        out->type = X28_CMD_PAR;
        return parse_ref_list(input, i + 4, len, out);
    }
    if (match_kw(input, i, len, "LANG", 4)) {
        out->type = X28_CMD_LANG;
        return capture_string_arg(input, i + 4, len, out);
    }
    if (match_kw(input, i, len, "HELP", 4)) {
        out->type = X28_CMD_HELP;
        return capture_string_arg(input, i + 4, len, out);
    }
    if (match_kw(input, i, len, "ID", 2)) {
        out->type = X28_CMD_NUI_ON;
        return capture_string_arg(input, i + 2, len, out);
    }

    /* Otherwise treat as a selection PAD command signal (§3.5.15) and
       parse its facility block (if present) + address. */
    out->type = X28_CMD_SELECTION;
    return parse_selection(input, i, len, out);
}

/* --- formatter helpers -------------------------------------------------- */

/* Write byte b at *pos in buf if there is space. Returns 0 on success,
   -1 on overflow. Always advances *pos so the caller's final length check
   sees the overrun. */
static int put_byte(uint8 *buf, uint32 cap, uint32 *pos, uint8 b)
{
    if (*pos >= cap) {
        (*pos)++;
        return -1;
    }
    buf[(*pos)++] = b;
    return 0;
}

static int put_str(uint8 *buf, uint32 cap, uint32 *pos, const char *s)
{
    int rc = 0;
    while (*s) {
        if (put_byte(buf, cap, pos, (uint8)*s) != 0) rc = -1;
        s++;
    }
    return rc;
}

static int put_u8(uint8 *buf, uint32 cap, uint32 *pos, uint8 v)
{
    char rev[3];
    uint8 r = 0;
    uint8 x = v;
    int rc = 0;

    if (x == 0) {
        return put_byte(buf, cap, pos, (uint8)'0');
    }
    while (x > 0) {
        rev[r++] = (char)('0' + (x % 10));
        x = (uint8)(x / 10);
    }
    while (r > 0) {
        r--;
        if (put_byte(buf, cap, pos, (uint8)rev[r]) != 0) rc = -1;
    }
    return rc;
}

static int put_effector(uint8 *buf, uint32 cap, uint32 *pos)
{
    int rc = 0;
    /* §3.5.2: CR LF. Padding per param 9 is appended by the line emitter. */
    if (put_byte(buf, cap, pos, IA5_CR) != 0) rc = -1;
    if (put_byte(buf, cap, pos, IA5_LF) != 0) rc = -1;
    return rc;
}

static int32 finalize(uint32 pos, uint32 cap, int rc)
{
    if (rc != 0 || pos > cap) return -1;
    return (int32)pos;
}

/* --- formatters --------------------------------------------------------- */

int32 x28_format_ack(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = put_effector(buf, buf_size, &pos);
    return finalize(pos, buf_size, rc);
}

int32 x28_format_par(const x28_param_pair_t *params, uint8 count,
                     uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    uint8 i;

    /* §3.5: non-exception service signals commence with and are followed
       by the format effector. */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    /* §3.5.14: <PAR> = "PAR ". */
    if (put_str(buf, buf_size, &pos, "PAR ") != 0) rc = -1;

    for (i = 0; i < count; i++) {
        if (i > 0) {
            if (put_str(buf, buf_size, &pos, ", ") != 0) rc = -1;
        }
        if (put_u8(buf, buf_size, &pos, params[i].ref) != 0) rc = -1;
        if (put_byte(buf, buf_size, &pos, IA5_COLON) != 0) rc = -1;
        if (params[i].invalid) {
            /* X.28 3.5.14: "the characters 4/9 (I) 4/14 (N) 5/6 (V) will be
               sent in place of the appropriate parameter value". */
            if (put_str(buf, buf_size, &pos, "INV") != 0) rc = -1;
        } else {
            if (put_u8(buf, buf_size, &pos, params[i].value) != 0) rc = -1;
        }
    }
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_err(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5: leading effector. §3.5.19: "ERR" followed by chars for further
       study; the trailing effector closes the signal. */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "ERR") != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_clr_indication(const char *cause_text,
                                uint8 cause_code, uint8 diagnostic,
                                uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5 leading effector, then §3.5.17.1: "CLR " <cause> " C:" <code>
       " D:" <diag>, then trailing effector. */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "CLR ") != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, cause_text) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, " C:") != 0) rc = -1;
    if (put_u8(buf, buf_size, &pos, cause_code) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, " D:") != 0) rc = -1;
    if (put_u8(buf, buf_size, &pos, diagnostic) != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_clr_confirmation(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5 leading effector + §3.5.9 + Table 7/X.28: <CLR><CONF><effector>.
       Standard format (no extended-dialogue text): "\r\nCLR CONF\r\n". */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "CLR CONF") != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_reset_indication(const char *cause_text, uint8 diagnostic,
                                  uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5 leading effector + §3.5.7: "RESET " <cause> " D:" <diag>. */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "RESET ") != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, cause_text) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, " D:") != 0) rc = -1;
    if (put_u8(buf, buf_size, &pos, diagnostic) != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_status_engaged(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5 leading effector + §3.5.11: <ENGAGED> = "ENGAGED". */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "ENGAGED") != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_status_free(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5 leading effector + §3.5.11: <FREE> = "FREE". */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "FREE") != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

int32 x28_format_connected(uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    /* §3.5.21: connected PAD service signal. v1.0 emits the minimal form:
       leading effector, "COM", trailing effector. Optional address /
       facility / call-user-data blocks are deferred. */
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, "COM") != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}

/* Case-insensitive equality on a NUL-terminated keyword and a subject
   string. Subject is matched after stripping leading whitespace. */
static int subject_is(const char *subject, const char *kw)
{
    uint32 i;
    while (*subject == ' ' || *subject == '\t') subject++;
    for (i = 0; kw[i] != '\0'; i++) {
        char a = subject[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != kw[i]) return 0;
    }
    /* Anything after the keyword (besides trailing whitespace) means
       the subject is a longer word, not a match. */
    while (subject[i] == ' ' || subject[i] == '\t') i++;
    return subject[i] == '\0';
}

int32 x28_format_help(const char *subject, uint8 *buf, uint32 buf_size)
{
    uint32 pos = 0;
    int rc = 0;
    const char *body;
    /* §5.5 + Table 10/X.28: PAD HELP service signal text is
       network-dependent and varies by subject. The Padawan-Lite subject
       set covers the standard X.28 commands; an empty subject falls
       through to the general one-liner. */
    if (subject == NULL) subject = "";

    if (subject_is(subject, "PAR") || subject_is(subject, "PAR?") ||
        subject_is(subject, "PARAMETER")) {
        body = "PAR?<refs> - read X.3 params, e.g. PAR?2,3,4. "
               "Bare PAR? reads all. See also: READ, SETREAD.";
    } else if (subject_is(subject, "SET")) {
        body = "SET <ref>:<val>[,...] - write X.3 params, "
               "e.g. SET 2:0, 3:0. Use SETREAD to set + readback.";
    } else if (subject_is(subject, "SET?") || subject_is(subject, "SETREAD") ||
               subject_is(subject, "RSETREAD")) {
        body = "SET?<refs>:<vals> - write then read; same as "
               "SET followed by PAR?.";
    } else if (subject_is(subject, "READ") || subject_is(subject, "RREAD")) {
        body = "READ - read all current X.3 parameter values "
               "(equivalent to PAR? with no refs).";
    } else if (subject_is(subject, "STAT") || subject_is(subject, "STATUS")) {
        body = "STAT - report call status: FREE (no call) "
               "or ENGAGED (call established).";
    } else if (subject_is(subject, "CLR") || subject_is(subject, "CLEAR")) {
        body = "CLR - clear the active call. Returns CLR CONF "
               "and emits the PAD waiting prompt.";
    } else if (subject_is(subject, "ICLR") || subject_is(subject, "ICLEAR")) {
        body = "ICLR - invitation to clear sent to the remote DTE "
               "(X.29). The remote should respond with a clear.";
    } else if (subject_is(subject, "INT") || subject_is(subject, "INTERRUPT")) {
        body = "INT - send an X.25 interrupt packet to the remote "
               "(1-byte user data, currently always 0).";
    } else if (subject_is(subject, "RESET")) {
        body = "RESET - reset the virtual call; data in transit "
               "may be lost. Stays in data-transfer state.";
    } else if (subject_is(subject, "PROF") || subject_is(subject, "PROFILE")) {
        body = "PROF <id> - load a standard profile into the X.3 "
               "param set. v1.0: 1=simple, 90=transparent.";
    } else if (subject_is(subject, "BREAK") || subject_is(subject, "BR")) {
        body = "BREAK - issue a break signal per X.28 4.11. Action "
               "set by X.3 param 7 bits.";
    } else if (subject_is(subject, "ID")) {
        body = "ID <nui> - set the session-level network user "
               "identification (X.28 5.2). Bare ID prompts for NUI.";
    } else if (subject_is(subject, "IDOFF")) {
        body = "IDOFF - clear the session-level NUI set by ID.";
    } else if (subject_is(subject, "LANG") || subject_is(subject, "LANGUAGE")) {
        body = "LANG <code> - select service-signal language. "
               "v1.0 supports English only.";
    } else if (subject_is(subject, "RPAR") || subject_is(subject, "RPAR?")) {
        body = "RPAR?<refs> - read the REMOTE PAD's X.3 params "
               "via X.29 PAD messages.";
    } else if (subject_is(subject, "RSET")) {
        body = "RSET <ref>:<val>[,...] - write the REMOTE PAD's X.3 "
               "params via X.29.";
    } else if (subject_is(subject, "CALL") || subject_is(subject, "SELECTION")) {
        body = "<address> or CALL <address> - place a call. Optionally "
               "prefix with facilities (e.g. Ndavid-30001) or trail "
               "with call user data (e.g. 30001DLOGIN).";
    } else if (subject_is(subject, "HELP")) {
        body = "HELP [<subject>] - show this help. Subject can be "
               "any PAD command keyword.";
    } else if (subject[0] == '\0') {
        body = "HELP: Padawan-Lite PAD. Commands: PAR? PROF SET SET? STAT "
               "RESET INT CLR ICLR RPAR? RSET BREAK ID IDOFF LANG "
               "HELP. Type HELP <command> for details.";
    } else {
        body = "HELP: subject not recognised. Try HELP with no "
               "argument for the command list.";
    }

    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    if (put_str(buf, buf_size, &pos, body) != 0) rc = -1;
    if (put_effector(buf, buf_size, &pos) != 0) rc = -1;
    return finalize(pos, buf_size, rc);
}
