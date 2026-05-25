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

/* PAD session state machine and command dispatcher (X.28 clauses 3, 4). */
#include "pad.h"
#include "personality.h"
#include "x28_signals.h"
#include "x29.h"
#include <string.h>

#define IA5_DLE 0x10
#define IA5_CR  0x0D
#define IA5_LF  0x0A
#define IA5_PLUS 0x2B

/* --- output helpers ----------------------------------------------------- */

/* Per-byte emit to DTE handling X.3 padding (params 9 / 14), line folding
   (param 10), and the page-wait LF counter (param 22). Gated by flow
   control (param 12 X-OFF state) and page-wait state - while either is
   active, all output is suppressed. Param 22's PAGE service signal is
   emitted inline at the moment the LF threshold is hit and any remaining
   bytes in the call are dropped (v1.2 deviation). */
static void to_dte(pad_session_t *p, const uint8 *data, uint32 len)
{
    static const uint8 padding_buf[255] = {0};
    uint8 cr_pad;
    uint8 lf_pad;
    uint8 fold;
    uint8 page;
    uint32 i;

    if (!p->emit_dte || len == 0) return;

    /* X.3 3.12 / X.28 4.14 + X.3 3.22 / X.28 4.18: while X-OFF or page
       wait is in effect the PAD does not transmit characters. */
    if (p->xoff_from_dte || p->page_wait_active) return;

    cr_pad = p->params.values[X3_PAR_CR_PAD];
    lf_pad = (p->state == PAD_STATE_DATA_TRANSFER)
                 ? p->params.values[X3_PAR_LF_PAD]
                 : p->params.values[X3_PAR_CR_PAD];
    fold = p->params.values[X3_PAR_FOLD];
    page = p->params.values[X3_PAR_PAGE];

    for (i = 0; i < len; i++) {
        uint8 c = data[i];

        /* X.3 3.10 / X.28 4.13: inject a format effector before a graphic
           char if the column counter has reached the configured limit. */
        if (fold > 0 && c >= 0x20 && c <= 0x7E &&
            p->fold_col >= (uint32)fold) {
            uint8 b;
            b = IA5_CR;
            p->emit_dte(p->ctx, &b, 1);
            if (cr_pad > 0) p->emit_dte(p->ctx, padding_buf, cr_pad);
            b = IA5_LF;
            p->emit_dte(p->ctx, &b, 1);
            if (lf_pad > 0) p->emit_dte(p->ctx, padding_buf, lf_pad);
            p->fold_col = 0;
            /* The auto-inserted LF is intentionally NOT counted toward
               the page-wait LF count. */
        }

        p->emit_dte(p->ctx, &c, 1);

        /* X.3 3.9 / 3.14: padding chars after CR / LF. */
        if (c == IA5_CR && cr_pad > 0) {
            p->emit_dte(p->ctx, padding_buf, cr_pad);
        } else if (c == IA5_LF && lf_pad > 0) {
            p->emit_dte(p->ctx, padding_buf, lf_pad);
        }

        if (c == IA5_CR) {
            p->fold_col = 0;
            /* X.28 §4.18.3: data forwarding conditions reset the LF count.
               CR resets here as a reasonable approximation. */
            p->lf_count = 0;
        } else if (c >= 0x20 && c <= 0x7E) {
            p->fold_col++;
        }

        if (c == IA5_LF) {
            p->lf_count++;
            /* X.3 3.22 / X.28 4.18.1: enter page wait after N linefeeds. */
            if (page > 0 && p->lf_count >= (uint32)page) {
                static const uint8 page_sig[] = {
                    IA5_CR, 'P', 'A', 'G', 'E'
                };
                p->page_wait_active = 1;
                p->emit_dte(p->ctx, page_sig, sizeof(page_sig));
                /* Drop any remaining bytes in this call until X-ON arrives.
                   v1.2 deviation: a real implementation would queue them. */
                return;
            }
        }
    }
}

static void to_remote(pad_session_t *p, const uint8 *data, uint32 len)
{
    if (p->emit_remote) {
        p->emit_remote(p->ctx, data, len);
    }
}

/* Forward-declared so echo_byte can consult the forwarding-char check
   when param 2 = 2 (echo all except the data forwarding sequence). */
static int is_forwarding_char(const pad_session_t *p, uint8 c);

/* Forward declared so the qbit-dispatch path in pad_input_remote can
   reach into the X.29 handler (and so RPAR/RSET/ICLR emission can call
   the X.29 message senders). */
static void dispatch_x29_message(pad_session_t *p, const x29_message_t *m);
static int  send_pad_msg(pad_session_t *p, const uint8 *body, uint32 len);

/* Forward declared so the X28_CMD_BREAK dispatch can request break actions
   with escape disabled (X.28 §5.1 NOTE). */
static int do_break_actions(pad_session_t *p, int escape_allowed);

/* Forward declared so SELECTION dispatch, pad_call_connected, and the
   post-command WAITING_FOR_CMD -> DATA_TRANSFER transition can all drain
   the remote queue on entry to data transfer. */
static void drain_remote_queue(pad_session_t *p);

/* X.3 3.20 echo mask: return 1 if character c should NOT be echoed under
   the current param 20 setting. Applies only when echo (param 2) = 1 per
   X.3 3.20 NOTE 5. */
static int is_masked_for_echo(const pad_session_t *p, uint8 c)
{
    uint8 mask = p->params.values[X3_PAR_MASK];
    int edit_active;
    if (mask == 0) return 0;

    if ((mask & 0x01) && c == 0x0D) return 1;                 /* CR */
    if ((mask & 0x02) && c == 0x0A) return 1;                 /* LF */
    if (mask & 0x04) {
        if (c == 0x0B || c == 0x09 || c == 0x0C) return 1;    /* VT HT FF */
    }
    if (mask & 0x08) {
        if (c == 0x07 || c == 0x08) return 1;                 /* BEL BS */
    }
    if (mask & 0x10) {
        if (c == 0x1B || c == 0x05) return 1;                 /* ESC ENQ */
    }
    if (mask & 0x20) {
        /* ACK NAK STX SOH EOT ETB ETX */
        if (c == 0x06 || c == 0x15 || c == 0x02 || c == 0x01 ||
            c == 0x04 || c == 0x17 || c == 0x03) return 1;
    }
    if (mask & 0x40) {
        /* Editing chars - only honoured when editing is active in the
           current state (X.3 3.20 NOTE 6). */
        edit_active = (p->state != PAD_STATE_DATA_TRANSFER) ||
                      (p->params.values[X3_PAR_EDIT] == 1);
        if (edit_active) {
            if (c == p->params.values[X3_PAR_CDEL]) return 1;
            if (c == p->params.values[X3_PAR_LDEL]) return 1;
            if (c == p->params.values[X3_PAR_LDIS]) return 1;
        }
    }
    if (mask & 0x80) {
        /* All other chars in cols 0,1 of IA5 not named in bits 1-5, and
           the DEL character. */
        if (c == 0x7F) return 1;
        if (c < 0x20) {
            switch (c) {
            case 0x01: case 0x02: case 0x03: case 0x04: case 0x05:
            case 0x06: case 0x07: case 0x08: case 0x09: case 0x0A:
            case 0x0B: case 0x0C: case 0x0D: case 0x15: case 0x17:
            case 0x1B:
                break; /* named in bits 1-5; bit 7 does not apply */
            default:
                return 1;
            }
        }
    }
    return 0;
}

static void echo_byte(pad_session_t *p, uint8 c)
{
    uint8 echo = p->params.values[X3_PAR_ECHO];
    uint8 out;
    if (echo == 0) return;
    if (echo == 1) {
        /* X.3 3.2 value 1: plain echo. Subject to param 20 echo mask. */
        if (is_masked_for_echo(p, c)) return;
        out = c;
    } else if (echo == 2) {
        /* X.3 3.2 value 2: echo all chars except the data forwarding
           sequence. v1.2 interprets the "data forwarding sequence" as the
           characters selected by param 3 (param 25 / extended forwarding
           signals is deferred). */
        if (is_forwarding_char(p, c)) return;
        out = c;
    } else if (echo >= 32 && echo <= 126) {
        /* X.3 3.2 value 32-126 (scrambled echo): emit the substitute char
           instead of the input char. */
        out = echo;
    } else {
        return;
    }
    to_dte(p, &out, 1);

    /* X.3 3.13 / X.28 4.15: when bit 2 (value 4) of param 13 is set and
       echo is enabled, insert LF after the echo of CR. */
    if (c == IA5_CR &&
        (p->params.values[X3_PAR_LF_INSERT] & 0x04)) {
        uint8 lf = IA5_LF;
        to_dte(p, &lf, 1);
    }
}

/* --- service signal emission ------------------------------------------- */

/* PAD service signals are suppressed entirely when X.3 param 6 = 0
   (X.28 3.5 and Table 1). */
static int signals_enabled(const pad_session_t *p)
{
    return p->params.values[X3_PAR_SIGNALS] != 0;
}

static void emit_ack(pad_session_t *p)
{
    uint8 buf[8];
    int32 n;
    if (!signals_enabled(p)) return;
    n = x28_format_ack(buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

/* Emit a personality-style "effector + text + effector" service signal:
   CR LF <text> CR LF. Used when a personality overrides the X.28
   abbreviation for a signal. */
static void emit_signal_text(pad_session_t *p, const char *text)
{
    uint8 buf[128];
    uint32 pos = 0;
    uint32 tlen;
    if (text == NULL) return;
    tlen = (uint32)strlen(text);
    if (tlen > sizeof(buf) - 4) tlen = (uint32)sizeof(buf) - 4;
    buf[pos++] = IA5_CR; buf[pos++] = IA5_LF;
    memcpy(buf + pos, text, tlen); pos += tlen;
    buf[pos++] = IA5_CR; buf[pos++] = IA5_LF;
    to_dte(p, buf, pos);
}

static void emit_err(pad_session_t *p)
{
    uint8 buf[16];
    int32 n;
    if (!signals_enabled(p)) return;
    if (p->personality && p->personality->err_text) {
        emit_signal_text(p, p->personality->err_text);
        return;
    }
    n = x28_format_err(buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

static void emit_status(pad_session_t *p)
{
    uint8 buf[16];
    int32 n;
    if (!signals_enabled(p)) return;
    if (p->personality) {
        const char *t = p->call.connected
                            ? p->personality->engaged_text
                            : p->personality->free_text;
        if (t != NULL) { emit_signal_text(p, t); return; }
    }
    n = p->call.connected
            ? x28_format_status_engaged(buf, sizeof(buf))
            : x28_format_status_free(buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

static void emit_par_pairs(pad_session_t *p,
                           const x28_param_pair_t *pairs, uint8 count)
{
    uint8 buf[512];
    int32 n;
    if (!signals_enabled(p)) return;
    n = x28_format_par(pairs, count, buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

static void emit_clr_confirm(pad_session_t *p)
{
    uint8 buf[16];
    int32 n;
    if (!signals_enabled(p)) return;
    n = x28_format_clr_confirmation(buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

/* X.28 §3.5.21 connected PAD service signal. Emitted on successful call
   setup in place of the bare-effector ACK previously used. */
static void emit_connected(pad_session_t *p)
{
    uint8 buf[16];
    int32 n;
    if (!signals_enabled(p)) return;
    if (p->personality && p->personality->connected_text) {
        emit_signal_text(p, p->personality->connected_text);
        return;
    }
    n = x28_format_connected(buf, sizeof(buf));
    if (n > 0) to_dte(p, buf, (uint32)n);
}

/* X.28 §3.1.3 + §3.5.23: emit the prompt PAD service signal when entering
   a "ready for command" state (PAD_WAITING or WAITING_FOR_CMD) and param 6
   has the prompt bit (value 4) set. Format: format effector followed by
   the character 2/10 (*). */
static void emit_prompt_if_enabled(pad_session_t *p)
{
    uint8 buf[3];
    uint8 ch = '*';                            /* X.28 default */
    if (!signals_enabled(p)) return;
    if (!(p->params.values[X3_PAR_SIGNALS] & 0x04)) return;
    if (p->personality && p->personality->prompt_char != 0) {
        ch = p->personality->prompt_char;
    }
    buf[0] = IA5_CR;
    buf[1] = IA5_LF;
    buf[2] = ch;
    to_dte(p, buf, 3);
}

/* §3.5.24 character deleted PAD service signal, controlled by param 19. */
static void emit_cdel_signal(pad_session_t *p)
{
    uint8 esig;
    if (!signals_enabled(p)) return;
    esig = p->params.values[X3_PAR_ESIG];
    if (esig == 0) return;
    if (esig == 1) {
        uint8 b = (uint8)'\\';            /* 5/12 */
        to_dte(p, &b, 1);
    } else if (esig == 2) {
        uint8 seq[3];
        seq[0] = 0x08; seq[1] = 0x20; seq[2] = 0x08; /* BS SP BS */
        to_dte(p, seq, 3);
    } else if (esig == 8 || (esig >= 32 && esig <= 126)) {
        to_dte(p, &esig, 1);
    }
}

/* §3.5.18 PAD identification PAD service signal. Format is network-
   dependent; per-session text is in pad_id_text. Per §3.5 this signal
   is not in the exception list, so leading and trailing format effectors
   are emitted. Suppressed when param 6 = 0 per §2.2.3 or when
   pad_id_text is empty. */
static void emit_pad_id(pad_session_t *p)
{
    uint32 text_len;
    uint8 buf[PAD_ID_MAX + 4];
    uint32 pos = 0;
    if (!signals_enabled(p)) return;
    text_len = (uint32)strlen(p->pad_id_text);
    if (text_len == 0) return;
    buf[pos++] = IA5_CR;
    buf[pos++] = IA5_LF;
    memcpy(buf + pos, p->pad_id_text, text_len);
    pos += text_len;
    buf[pos++] = IA5_CR;
    buf[pos++] = IA5_LF;
    to_dte(p, buf, pos);
}

/* §3.5.25 line deleted PAD service signal, controlled by param 19. */
static void emit_ldel_signal(pad_session_t *p, uint32 chars_deleted)
{
    uint8 esig;
    if (!signals_enabled(p)) return;
    esig = p->params.values[X3_PAR_ESIG];
    if (esig == 0) return;
    if (esig == 1 || esig == 8 || (esig >= 32 && esig <= 126)) {
        uint8 sig[5];
        sig[0] = 'X'; sig[1] = 'X'; sig[2] = 'X';
        sig[3] = 0x0D; sig[4] = 0x0A;
        to_dte(p, sig, 5);
    } else if (esig == 2) {
        uint32 i;
        uint8 seq[3];
        seq[0] = 0x08; seq[1] = 0x20; seq[2] = 0x08;
        for (i = 0; i < chars_deleted; i++) {
            to_dte(p, seq, 3);
        }
    }
}

/* --- command dispatch -------------------------------------------------- */

/* X.28 3.3.2 + 3.5.14: SET accepts valid pairs from a mixed batch and reports
   invalid pairs by setting the .invalid flag on their entry in the response
   PAR signal. Returns 1 if any pair was invalid, 0 otherwise. The output
   array `out` is populated with one entry per input pair. */
static int classify_set_batch(pad_session_t *p,
                              const x28_param_pair_t *in, uint8 count,
                              x28_param_pair_t *out)
{
    int any_invalid = 0;
    uint8 i;
    for (i = 0; i < count; i++) {
        out[i].ref = in[i].ref;
        out[i].value = in[i].value;
        out[i].invalid = 0;
        if (x3_validate(in[i].ref, in[i].value) != X3_OK) {
            out[i].invalid = 1;
            any_invalid = 1;
        } else {
            x3_set(&p->params, in[i].ref, in[i].value);
        }
    }
    return any_invalid;
}

static void dispatch_command(pad_session_t *p, const x28_command_t *cmd)
{
    switch (cmd->type) {
    case X28_CMD_PAR: {
        x28_param_pair_t out[X28_MAX_PARAMS];
        uint8 count = 0;
        uint8 i;
        if (cmd->param_count == 0) {
            /* X.28 3.5.4: empty list = all parameters. */
            for (i = X3_PAR_MIN;
                 i <= X3_PAR_MAX && count < X28_MAX_PARAMS;
                 i++) {
                out[count].ref = i;
                out[count].value = p->params.values[i];
                out[count].invalid = 0;
                count++;
            }
        } else {
            for (i = 0;
                 i < cmd->param_count && count < X28_MAX_PARAMS;
                 i++) {
                out[count].ref = cmd->params[i].ref;
                out[count].value = p->params.values[cmd->params[i].ref];
                out[count].invalid = 0;
                count++;
            }
        }
        emit_par_pairs(p, out, count);
        break;
    }

    case X28_CMD_SET: {
        x28_param_pair_t out[X28_MAX_PARAMS];
        int any_invalid = classify_set_batch(p, cmd->params,
                                             cmd->param_count, out);
        if (any_invalid) {
            emit_par_pairs(p, out, cmd->param_count);
        } else {
            emit_ack(p);
        }
        break;
    }

    case X28_CMD_SET_READ: {
        /* SET? always responds with a PAR signal (X.28 3.3.2). For valid
           pairs the response carries the freshly-stored value; for invalid
           pairs the response carries the request value flagged INV. */
        x28_param_pair_t out[X28_MAX_PARAMS];
        uint8 i;
        for (i = 0; i < cmd->param_count; i++) {
            out[i].ref = cmd->params[i].ref;
            if (x3_validate(cmd->params[i].ref,
                            cmd->params[i].value) == X3_OK) {
                x3_set(&p->params, cmd->params[i].ref, cmd->params[i].value);
                out[i].value = p->params.values[cmd->params[i].ref];
                out[i].invalid = 0;
            } else {
                out[i].value = cmd->params[i].value;
                out[i].invalid = 1;
            }
        }
        emit_par_pairs(p, out, cmd->param_count);
        break;
    }

    case X28_CMD_PROF:
        if (x3_load_profile(&p->params, cmd->profile_id) == X3_OK) {
            emit_ack(p);
        } else {
            emit_err(p);
        }
        break;

    case X28_CMD_STAT:
        emit_status(p);
        break;

    case X28_CMD_CLR:
        if (p->call.connected) {
            x25_clear(&p->call, 0, 0);
            emit_clr_confirm(p);
            p->state = PAD_STATE_PAD_WAITING;
        } else {
            /* §3.2.3.1.3: clear request in PAD waiting state is invalid. */
            emit_err(p);
        }
        break;

    case X28_CMD_RESET:
        if (p->call.connected) {
            x25_reset(&p->call, 0, 0);
            emit_ack(p);
        } else {
            emit_err(p);
        }
        break;

    case X28_CMD_INT:
        if (p->call.connected) {
            x25_interrupt(&p->call, 0);
            emit_ack(p);
        } else {
            emit_err(p);
        }
        break;

    case X28_CMD_SELECTION:
        if (p->call.connected) {
            emit_err(p);
        } else if (p->auth_cb != NULL &&
                   p->auth_cb(p->auth_ctx,
                              (const struct x28_facility_t *)cmd->facilities,
                              cmd->facility_count,
                              cmd->address,
                              p->session_nui) != 0) {
            /* Auth rejected: emit CLR with cause "NA" (access barred,
               Table 6/X.28) and stay in PAD_WAITING. */
            if (signals_enabled(p)) {
                uint8 buf[64];
                int32 n = x28_format_clr_indication("NA", 0, 0,
                                                    buf, sizeof(buf));
                if (n > 0) to_dte(p, buf, (uint32)n);
            }
        } else {
            int rc = x25_call(&p->call, cmd->address);
            if (rc == X25_OK) {
                /* Synchronous success - skip CONN_IN_PROGRESS straight to
                   data transfer and emit the §3.5.21 connected signal. */
                emit_connected(p);
                p->state = PAD_STATE_DATA_TRANSFER;
                p->idle_ticks = 0;
                drain_remote_queue(p);
            } else if (rc == X25_IN_PROGRESS) {
                /* Async setup: enter CONN_IN_PROGRESS and wait for the
                   X.25 layer to fire pad_call_connected (or
                   pad_remote_cleared on failure). No DTE output yet. */
                p->state = PAD_STATE_CONN_IN_PROGRESS;
            } else {
                emit_err(p);
            }
        }
        break;

    case X28_CMD_RPAR: {
        /* §3.5.10: RPAR? emits an X.29 Read PAD message to the remote.
           The reply (Parameter indication) arrives later via
           pad_input_remote(qbit=1) and is surfaced as a PAR service
           signal by dispatch_x29_message. */
        uint8 buf[1 + X29_MAX_PAIRS];
        uint8 refs[X29_MAX_PAIRS];
        uint8 count = cmd->param_count;
        uint8 i2;
        int32 n2;
        if (!p->call.connected) { emit_err(p); break; }
        if (count > X29_MAX_PAIRS) count = X29_MAX_PAIRS;
        for (i2 = 0; i2 < count; i2++) refs[i2] = cmd->params[i2].ref;
        n2 = x29_encode_read(refs, count, buf, sizeof(buf));
        if (n2 < 0) { emit_err(p); break; }
        (void)send_pad_msg(p, buf, (uint32)n2);
        emit_ack(p);
        break;
    }

    case X28_CMD_RSET: {
        /* §3.5.12: RSET emits an X.29 Set-and-read PAD message so the
           remote both applies the values and confirms them. */
        x29_pair_t pairs[X29_MAX_PAIRS];
        uint8 buf[1 + X29_MAX_PAIRS * 2];
        uint8 count = cmd->param_count;
        uint8 i2;
        int32 n2;
        if (!p->call.connected) { emit_err(p); break; }
        if (count > X29_MAX_PAIRS) count = X29_MAX_PAIRS;
        for (i2 = 0; i2 < count; i2++) {
            pairs[i2].ref   = cmd->params[i2].ref;
            pairs[i2].value = cmd->params[i2].value;
        }
        n2 = x29_encode_set_read(pairs, count, buf, sizeof(buf));
        if (n2 < 0) { emit_err(p); break; }
        (void)send_pad_msg(p, buf, (uint32)n2);
        emit_ack(p);
        break;
    }

    case X28_CMD_ICLR: {
        /* §3.5.8.2: ICLR emits an X.29 Invitation to clear PAD message.
           The remote chooses whether to actually clear; we just ack
           the local command. */
        uint8 buf[2];
        int32 n2;
        if (!p->call.connected) { emit_err(p); break; }
        n2 = x29_encode_invite_clear(buf, sizeof(buf));
        if (n2 < 0) { emit_err(p); break; }
        (void)send_pad_msg(p, buf, (uint32)n2);
        emit_ack(p);
        break;
    }

    case X28_CMD_BREAK:
        /* §5.1: act as if a break signal had been received, but escape
           from data transfer is explicitly NOT supported via this
           command (per the NOTE in §5.1). */
        do_break_actions(p, 0);
        emit_ack(p);
        break;

    case X28_CMD_NUI_ON: {
        /* §5.2: ID <nui>. When the NUI string is supplied inline,
           persist it directly. When the user typed just "ID" with no
           argument, the spec calls for the PAD to prompt for the NUI
           string; we emit a NUI? prompt and arm awaiting_nui so the
           next CR-terminated input is captured as the value instead
           of being parsed as a new command. */
        if (cmd->address_len == 0) {
            if (signals_enabled(p)) {
                const char *text = "NUI?";
                if (p->personality && p->personality->nui_prompt) {
                    text = p->personality->nui_prompt;
                }
                emit_signal_text(p, text);
            }
            p->awaiting_nui = 1;
        } else {
            uint32 n = cmd->address_len;
            if (n > PAD_NUI_MAX) n = PAD_NUI_MAX;
            memcpy(p->session_nui, cmd->address, n);
            p->session_nui[n] = '\0';
            emit_ack(p);
        }
        break;
    }

    case X28_CMD_NUI_OFF:
        /* §5.2: IDOFF - clear the session-level NUI. */
        p->session_nui[0] = '\0';
        emit_ack(p);
        break;

    case X28_CMD_LANG:
        /* §5.3: LANGUAGE selection. v1.2 does not actually switch the
           service-signal text language (would set param 6's high nibble).
           Accept and acknowledge. See deviations.txt. */
        emit_ack(p);
        break;

    case X28_CMD_HELP: {
        /* §5.4 / §5.5: emit help PAD service signal. */
        uint8 buf[256];
        int32 n;
        if (signals_enabled(p)) {
            n = x28_format_help(cmd->address, buf, sizeof(buf));
            if (n > 0) to_dte(p, buf, (uint32)n);
        }
        break;
    }

    case X28_CMD_UNKNOWN:
    default:
        emit_err(p);
        break;
    }
}

/* --- editing (X.28 3.6) ------------------------------------------------ */

/* Apply editing functions (X.28 §3.6) to the buffer pointed to by buf
   and length pointed to by len_ptr. Used by both the command-state edit
   buffer and (when X.3 param 15 = 1, X.28 §4.17) the data-transfer
   assembly buffer. Returns 1 if c was consumed. */
static int apply_editing_to(pad_session_t *p, uint8 *buf,
                            uint32 *len_ptr, uint8 c)
{
    if (c == p->params.values[X3_PAR_CDEL]) {
        if (*len_ptr > 0) {
            (*len_ptr)--;
            emit_cdel_signal(p);
        }
        return 1;
    }
    if (c == p->params.values[X3_PAR_LDEL]) {
        uint32 had = *len_ptr;
        *len_ptr = 0;
        if (had > 0) {
            emit_ldel_signal(p, had);
        }
        return 1;
    }
    if (c == p->params.values[X3_PAR_LDIS]) {
        if (*len_ptr > 0) {
            to_dte(p, buf, *len_ptr);
        }
        return 1;
    }
    return 0;
}

static int apply_editing(pad_session_t *p, uint8 c)
{
    return apply_editing_to(p, p->edit_buf, &p->edit_len, c);
}

/* --- input from DTE ---------------------------------------------------- */

/* Feed one byte while in PAD command / waiting / waiting-for-cmd states. */
static void feed_command_byte(pad_session_t *p, uint8 c)
{
    /* X.28 §3.2.1.3: receiving the first character of a PAD command
       signal while in PAD waiting state transitions the interface to
       PAD_COMMAND. Without this, the post-dispatch transition below
       never moves out of PAD_WAITING, and the param-6 prompt is
       never emitted after a command. CR / '+' on an empty buffer is
       not a "first character" - it terminates an already-empty
       (no-op) command. */
    if (p->state == PAD_STATE_PAD_WAITING &&
        c != IA5_CR && c != IA5_PLUS) {
        p->state = PAD_STATE_PAD_COMMAND;
    }

    /* PAD command delimiter (§3.5.1): CR or '+'. */
    if (c == IA5_CR || c == IA5_PLUS) {
        echo_byte(p, c);
        if (p->awaiting_nui) {
            /* §5.2: bare ID was answered with this input - it's the
               NUI value, not a new command. Empty answer cancels the
               prompt without changing the stored NUI. */
            if (p->edit_len > 0) {
                uint32 n = p->edit_len;
                if (n > PAD_NUI_MAX) n = PAD_NUI_MAX;
                memcpy(p->session_nui, p->edit_buf, n);
                p->session_nui[n] = '\0';
            }
            p->awaiting_nui = 0;
            emit_ack(p);
        } else if (p->edit_len > 0) {
            x28_command_t cmd;
            int rc = x28_parse_command((const char *)p->edit_buf,
                                       p->edit_len, &cmd);
            if (rc == X28_PARSE_OK) {
                dispatch_command(p, &cmd);
            } else {
                emit_err(p);
            }
        }
        p->edit_len = 0;

        /* Return to the right idle state. Dispatch may already have moved
           us out of the command-editing states. */
        if (p->state == PAD_STATE_PAD_COMMAND) {
            p->state = PAD_STATE_PAD_WAITING;
            emit_prompt_if_enabled(p);
        } else if (p->state == PAD_STATE_WAITING_FOR_CMD) {
            /* §3.2.1.5: return to whichever state the recall escaped
               from (DATA_TRANSFER if the call is up, CONN_IN_PROGRESS
               if the call is still being set up). Drop to PAD_WAITING
               only if the call has gone away in the meantime (e.g. CLR
               while in DATA_TRANSFER). */
            if (p->call.connected) {
                p->state = PAD_STATE_DATA_TRANSFER;
                p->idle_ticks = 0;
                drain_remote_queue(p);
            } else if (p->pre_recall_state == PAD_STATE_CONN_IN_PROGRESS) {
                p->state = PAD_STATE_CONN_IN_PROGRESS;
            } else {
                p->state = PAD_STATE_PAD_WAITING;
                emit_prompt_if_enabled(p);
            }
            p->pre_recall_state = PAD_STATE_PAD_WAITING; /* reset */
        }
        return;
    }

    if (apply_editing(p, c)) {
        return;
    }

    if (p->edit_len < PAD_EDIT_BUF_SIZE) {
        p->edit_buf[p->edit_len++] = c;
        echo_byte(p, c);
    }
    /* else: buffer overflow handling (emit ERR? clear?) is deferred. */
}

/* X.3 3.3 forwarding-char mask: a bitwise-OR of the basic functions
   defined in Table 1/X.3. */
static int is_forwarding_char(const pad_session_t *p, uint8 c)
{
    uint8 mask = p->params.values[X3_PAR_FORWARD];
    if (mask == 0) return 0;

    /* Bit 0 (value 1): alphanumeric A-Z, a-z, 0-9. */
    if (mask & 0x01) {
        if ((c >= 'A' && c <= 'Z') ||
            (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9')) return 1;
    }
    /* Bit 1 (value 2): CR. */
    if ((mask & 0x02) && c == IA5_CR) return 1;
    /* Bit 2 (value 4): ESC, BEL, ENQ, ACK. */
    if (mask & 0x04) {
        if (c == 0x1B || c == 0x07 || c == 0x05 || c == 0x06) return 1;
    }
    /* Bit 3 (value 8): DEL, CAN, DC2. */
    if (mask & 0x08) {
        if (c == 0x7F || c == 0x18 || c == 0x12) return 1;
    }
    /* Bit 4 (value 16): ETX, EOT. */
    if (mask & 0x10) {
        if (c == 0x03 || c == 0x04) return 1;
    }
    /* Bit 5 (value 32): HT, LF, VT, FF. */
    if (mask & 0x20) {
        if (c == 0x09 || c == 0x0A || c == 0x0B || c == 0x0C) return 1;
    }
    /* Bit 6 (value 64): all other chars in cols 0,1 of IA5 not named in
       bits 1-5 above. CR is excluded (bit 1) along with BEL/ENQ/ACK/ESC
       (bit 2), CAN/DC2 (bit 3, DEL is in col 7), ETX/EOT (bit 4) and
       HT/LF/VT/FF (bit 5). */
    if ((mask & 0x40) && c < 0x20) {
        switch (c) {
        case 0x03: case 0x04: case 0x05: case 0x06: case 0x07:
        case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D:
        case 0x12: case 0x18: case 0x1B:
            break; /* named in bits 1-5, only those bits fire */
        default:
            return 1;
        }
    }
    return 0;
}

static int is_recall_char(const pad_session_t *p, uint8 c)
{
    uint8 recall = p->params.values[X3_PAR_RECALL];
    if (recall == 1 && c == IA5_DLE) return 1;
    if (recall >= 32 && recall <= 126 && c == recall) return 1;
    return 0;
}

static void flush_assembly(pad_session_t *p)
{
    if (p->asm_len > 0) {
        to_remote(p, p->asm_buf, p->asm_len);
        p->asm_len = 0;
    }
}

/* X.28 §2.2 handshake completion: emit PAD identification (§3.5.18) and
   advance through DTE_WAITING -> SERVICE_READY -> PAD_WAITING. The
   intermediate states are crossed in one synchronous call; they exist in
   the enum for completeness and for future async work (e.g. modelling
   the §2.2.5 fault timeout). */
static void complete_handshake(pad_session_t *p)
{
    p->state = PAD_STATE_DTE_WAITING;
    p->state = PAD_STATE_SERVICE_READY;
    emit_pad_id(p);
    p->state = PAD_STATE_PAD_WAITING;
    emit_prompt_if_enabled(p);
}

/* Feed one byte while in ACTIVE_LINK or SERVICE_REQUEST. The byte is
   consumed by the handshake (not forwarded to the command parser).
   X.28 §2.2 envisages the PAD using the byte timing and values to
   auto-detect line speed / code / parity; v1.2 has neither capability
   (we're on a logical line) so the bytes are simply discarded apart
   from CR, which signals end of the service request. */
static void feed_service_request_byte(pad_session_t *p, uint8 c)
{
    if (p->state == PAD_STATE_ACTIVE_LINK) {
        p->state = PAD_STATE_SERVICE_REQUEST;
    }
    if (c == 0x0D) {
        complete_handshake(p);
    }
}

static void feed_data_byte(pad_session_t *p, uint8 c)
{
    /* X.28 4.9 / X.3 3.1: PAD recall escapes to PAD command entry. */
    if (is_recall_char(p, c)) {
        p->pre_recall_state = PAD_STATE_DATA_TRANSFER;
        p->state = PAD_STATE_WAITING_FOR_CMD;
        return;
    }

    /* X.3 3.12 / X.28 4.14: param 12 = 1 enables X-ON/X-OFF flow control
       from the DTE. X.3 3.22 / X.28 4.18.2: X-ON (DC1) cancels page wait
       independently of param 12. Per spec these chars are never echoed
       and never forwarded. */
    if (c == 0x11) { /* DC1 / X-ON */
        if (p->page_wait_active) {
            uint8 effector[2];
            p->page_wait_active = 0;
            p->lf_count = 0;
            effector[0] = IA5_CR;
            effector[1] = IA5_LF;
            p->emit_dte(p->ctx, effector, 2);
            drain_remote_queue(p);
            return;
        }
        if (p->params.values[X3_PAR_FLOW] == 1) {
            if (p->xoff_from_dte) {
                p->xoff_from_dte = 0;
                drain_remote_queue(p);
            }
            return;
        }
    } else if (c == 0x13) { /* DC3 / X-OFF */
        if (p->params.values[X3_PAR_FLOW] == 1) {
            p->xoff_from_dte = 1;
            return;
        }
    }

    /* X.3 3.4: the idle timer measures gaps between successive DTE chars.
       Any byte resets it. */
    p->idle_ticks = 0;

    /* X.28 §4.17 / X.3 3.15: when param 15 = 1 the editing functions
       (cdel, ldel, ldis) also operate on the assembly buffer during the
       data transfer state. */
    if (p->params.values[X3_PAR_EDIT] == 1) {
        if (apply_editing_to(p, p->asm_buf, &p->asm_len, c)) {
            return;
        }
    }

    echo_byte(p, c);

    if (p->asm_len < PAD_ASM_BUF_SIZE) {
        p->asm_buf[p->asm_len++] = c;
    }

    /* X.3 3.13 / X.28 4.15 bit 1 (value 2): insert LF after every CR in
       the data stream FROM the DTE. LF lands in the assembly buffer
       alongside the CR so they get forwarded together when the CR
       triggers a forwarding flush. If param 15 = 1, a subsequent cdel
       would remove the LF first then the CR -- consistent with the
       spec note that the inserted LF is editable. */
    if (c == IA5_CR &&
        (p->params.values[X3_PAR_LF_INSERT] & 0x02) &&
        p->asm_len < PAD_ASM_BUF_SIZE) {
        p->asm_buf[p->asm_len++] = IA5_LF;
    }

    /* X.3 3.5: when ancillary device control is enabled (param 5 > 0)
       emit X-OFF to the DTE as the assembly buffer approaches capacity.
       v1.2 simple high-watermark at 80% full; X-ON sent after the
       next flush. Rarely triggers since forwarding usually keeps the
       buffer well below the mark. */
    if (p->params.values[X3_PAR_DEVICE] > 0 &&
        !p->xoff_to_dte &&
        p->asm_len * 5 >= PAD_ASM_BUF_SIZE * 4) {
        uint8 xoff = 0x13;
        p->emit_dte(p->ctx, &xoff, 1);
        p->xoff_to_dte = 1;
    }

    if (is_forwarding_char(p, c) || p->asm_len >= PAD_ASM_BUF_SIZE) {
        flush_assembly(p);
        if (p->xoff_to_dte) {
            uint8 xon = 0x11;
            p->emit_dte(p->ctx, &xon, 1);
            p->xoff_to_dte = 0;
        }
    }
}

/* --- public entry points ----------------------------------------------- */

int pad_init(pad_session_t *p, uint8 profile_id,
             pad_emit_fn dte_cb, pad_emit_fn remote_cb, void *ctx)
{
    int rc;
    memset(p, 0, sizeof(*p));
    rc = x3_load_profile(&p->params, profile_id);
    if (rc != X3_OK) return rc;
    p->state = PAD_STATE_PAD_WAITING;
    p->emit_dte = dte_cb;
    p->emit_remote = remote_cb;
    p->ctx = ctx;
    /* Default PAD identification (§3.5.18); override via
       pad_set_identification before issuing the handshake. */
    strcpy(p->pad_id_text, "PAD");
    return 0;
}

void pad_set_auth_callback(pad_session_t *p, pad_auth_fn cb, void *ctx)
{
    p->auth_cb  = cb;
    p->auth_ctx = ctx;
}

void pad_set_personality(pad_session_t *p, const personality_t *pers)
{
    p->personality = pers;
    if (pers != NULL) {
        personality_apply_profile_overlay(pers, &p->params);
        if (pers->banner != NULL) {
            pad_set_identification(p, pers->banner);
        }
    }
}

void pad_set_trace_callback(pad_session_t *p, pad_trace_fn cb, void *ctx)
{
    p->trace_cb  = cb;
    p->trace_ctx = ctx;
}

void pad_set_identification(pad_session_t *p, const char *text)
{
    uint32 n;
    if (text == NULL) {
        p->pad_id_text[0] = '\0';
        return;
    }
    n = (uint32)strlen(text);
    if (n > PAD_ID_MAX) n = PAD_ID_MAX;
    memcpy(p->pad_id_text, text, n);
    p->pad_id_text[n] = '\0';
}

int pad_flush(pad_session_t *p)
{
    if (p->state != PAD_STATE_DATA_TRANSFER) return 0;
    flush_assembly(p);
    return 0;
}

/* --- remote-initiated events ------------------------------------------- */

static const char *clear_cause_text(pad_clear_cause_t c)
{
    switch (c) {
    case PAD_CLR_NUMBER_BUSY:              return "OCC";
    case PAD_CLR_NETWORK_PROBLEM:          return "NC";
    case PAD_CLR_INVALID_FACILITY:         return "INV";
    case PAD_CLR_ACCESS_BARRED:            return "NA";
    case PAD_CLR_LOCAL_PROCEDURE_ERROR:    return "ERR";
    case PAD_CLR_REMOTE_PROCEDURE_ERROR:   return "RPE";
    case PAD_CLR_NUMBER_NOT_ASSIGNED:      return "NP";
    case PAD_CLR_NUMBER_OUT_OF_ORDER:      return "OOO";
    case PAD_CLR_REMOTE_REQUEST:           return "DTE";
    case PAD_CLR_REMOTE_DEVICE_ERROR:      return "DER";
    case PAD_CLR_REVERSE_CHARGING_REFUSED: return "RCH";
    case PAD_CLR_INCOMPATIBLE_DESTINATION: return "ID";
    case PAD_CLR_SHIP_NOT_CONTACTED:       return "SHN";
    case PAD_CLR_FAST_SELECT_REFUSED:      return "FNA";
    case PAD_CLR_CANNOT_ROUTE:             return "RNA";
    }
    return "ERR";
}

static const char *reset_cause_text(pad_reset_cause_t c)
{
    switch (c) {
    case PAD_RESET_REMOTE_DEVICE:          return "DTE";
    case PAD_RESET_LOCAL_PROCEDURE_ERROR:  return "ERR";
    case PAD_RESET_NETWORK_PROBLEM:        return "NC";
    case PAD_RESET_REMOTE_PROCEDURE_ERROR: return "RPE";
    }
    return "ERR";
}

int pad_remote_cleared(pad_session_t *p, pad_clear_cause_t cause,
                       uint8 cause_code, uint8 diagnostic)
{
    if (signals_enabled(p)) {
        uint8       buf[64];
        const char *text = NULL;
        int32       n;
        /* Personality may override the per-cause abbreviation. Bounds-
           check against PERSONALITY_CLR_CAUSE_COUNT since pad_clear_cause_t
           is an int. */
        if (p->personality &&
            (uint32)cause < PERSONALITY_CLR_CAUSE_COUNT &&
            p->personality->clr_text[cause] != NULL) {
            text = p->personality->clr_text[cause];
        } else {
            text = clear_cause_text(cause);
        }
        n = x28_format_clr_indication(text, cause_code, diagnostic,
                                      buf, sizeof(buf));
        if (n > 0) to_dte(p, buf, (uint32)n);
    }
    p->call.connected = 0;
    p->call.call_id = 0;
    p->asm_len = 0;
    p->edit_len = 0;
    p->idle_ticks = 0;
    /* Drop any queued remote bytes - they belong to the now-cleared call
       and must not leak into a future call on this session. */
    p->remote_q_len = 0;
    /* Same for any DTE bytes buffered during a CONN_IN_PROGRESS that
       just failed: they belonged to the call that's now gone. */
    p->pending_dte_len = 0;
    p->state = PAD_STATE_PAD_WAITING;
    emit_prompt_if_enabled(p);
    return 0;
}

int pad_remote_reset(pad_session_t *p, pad_reset_cause_t cause,
                     uint8 diagnostic)
{
    if (signals_enabled(p)) {
        uint8 buf[64];
        int32 n = x28_format_reset_indication(reset_cause_text(cause),
                                              diagnostic,
                                              buf, sizeof(buf));
        if (n > 0) to_dte(p, buf, (uint32)n);
    }
    /* X.28 Table 5: reset indications say "data may be lost"; drop the
       assembly buffer since in-flight bytes have not been delivered. */
    p->asm_len = 0;
    p->idle_ticks = 0;
    /* Call remains up; state stays DATA_TRANSFER (or wherever it was). */
    return 0;
}

int pad_remote_interrupted(pad_session_t *p, uint8 user_data)
{
    /* X.28 leaves the response to a remote-originated interrupt
       network-dependent ("for further study"). X.29 §3 allows the
       PAD to indicate the event to the DTE and/or discard buffered
       output. v1.2 emits BEL (0x07) to the DTE when service signals
       are enabled and we're in DATA_TRANSFER; the 1-byte user_data
       field of the X.25 interrupt packet is not currently surfaced.
       Buffered output is NOT discarded -- the PAD-side decision to
       discard would belong with the X.29 layer. */
    (void)user_data;
    if (p->state == PAD_STATE_DATA_TRANSFER && signals_enabled(p)) {
        uint8 bel = 0x07;
        to_dte(p, &bel, 1);
    }
    return 0;
}

int pad_call_connected(pad_session_t *p)
{
    uint32 i;
    uint32 n;
    if (p->state != PAD_STATE_CONN_IN_PROGRESS) return -1;
    p->call.connected = 1;
    emit_connected(p);
    p->state = PAD_STATE_DATA_TRANSFER;
    p->idle_ticks = 0;
    drain_remote_queue(p);
    /* Replay any DTE bytes buffered during call setup through the
       normal data-transfer feeder, so echo / forwarding / idle-timer
       all apply correctly. */
    n = p->pending_dte_len;
    p->pending_dte_len = 0;
    for (i = 0; i < n; i++) {
        feed_data_byte(p, p->pending_dte[i]);
    }
    return 0;
}

int pad_init_handshake(pad_session_t *p, uint8 profile_id,
                       pad_emit_fn dte_cb, pad_emit_fn remote_cb,
                       void *ctx)
{
    int rc = pad_init(p, profile_id, dte_cb, remote_cb, ctx);
    if (rc != 0) return rc;
    p->state = PAD_STATE_ACTIVE_LINK;
    return 0;
}

int pad_input_dte(pad_session_t *p, const uint8 *data, uint32 len)
{
    uint32 i;
    if (p->trace_cb != NULL && len > 0) {
        p->trace_cb(p->trace_ctx, PAD_TRACE_DTE, data, len);
    }
    for (i = 0; i < len; i++) {
        uint8 c = data[i];
        switch (p->state) {
        case PAD_STATE_ACTIVE_LINK:
        case PAD_STATE_SERVICE_REQUEST:
            feed_service_request_byte(p, c);
            break;
        case PAD_STATE_PAD_WAITING:
        case PAD_STATE_PAD_COMMAND:
        case PAD_STATE_WAITING_FOR_CMD:
            feed_command_byte(p, c);
            break;
        case PAD_STATE_DATA_TRANSFER:
            feed_data_byte(p, c);
            break;
        case PAD_STATE_CONN_IN_PROGRESS:
            /* §3.2.1.5: PAD recall during call setup escapes to
               WAITING_FOR_CMD; the spec drops all other bytes here.
               Padawan-Lite extension: buffer non-recall bytes (up to
               PAD_PENDING_SIZE) and replay them on entry to
               DATA_TRANSFER, so a user typing into a fast TCP-backed
               call doesn't lose keystrokes. Recall flushes the buffer
               since the user has shifted focus. */
            if (is_recall_char(p, c)) {
                p->pre_recall_state = PAD_STATE_CONN_IN_PROGRESS;
                p->state = PAD_STATE_WAITING_FOR_CMD;
                p->pending_dte_len = 0;
            } else if (p->pending_dte_len < PAD_PENDING_SIZE) {
                p->pending_dte[p->pending_dte_len++] = c;
            }
            break;
        case PAD_STATE_DTE_WAITING:
        case PAD_STATE_SERVICE_READY:
            /* Transient pre-PAD-waiting states; bytes arriving during them
               would be discarded by the spec. In v1.2 these states are not
               held across input boundaries (complete_handshake runs to
               completion in one call), so this branch is unreachable in
               practice. */
            break;
        default:
            break;
        }
    }
    return 0;
}

/* Emit remote-side bytes to the DTE applying param 8 (discard) and
   X.3 3.13 / X.28 4.15 bit 0 (LF after CR in stream to DTE). Used by
   both pad_input_remote (in DATA_TRANSFER) and drain_remote_queue. */
static void emit_remote_to_dte(pad_session_t *p,
                               const uint8 *data, uint32 len)
{
    if (p->params.values[X3_PAR_DISCARD] != 0) return;
    if (p->params.values[X3_PAR_LF_INSERT] & 0x01) {
        uint32 i;
        uint32 run_start = 0;
        uint8 lf = IA5_LF;
        for (i = 0; i < len; i++) {
            if (data[i] == IA5_CR) {
                to_dte(p, data + run_start, i - run_start + 1);
                to_dte(p, &lf, 1);
                run_start = i + 1;
            }
        }
        if (run_start < len) {
            to_dte(p, data + run_start, len - run_start);
        }
    } else {
        to_dte(p, data, len);
    }
}

static void queue_remote(pad_session_t *p, const uint8 *data, uint32 len)
{
    uint32 room = PAD_REMOTE_Q_SIZE - p->remote_q_len;
    uint32 take = (len < room) ? len : room;
    if (take > 0) {
        memcpy(p->remote_q + p->remote_q_len, data, take);
        p->remote_q_len += take;
    }
    /* Overflow (take < len): bytes silently dropped; see deviations.txt. */
}

static void drain_remote_queue(pad_session_t *p)
{
    uint32 n;
    if (p->state != PAD_STATE_DATA_TRANSFER) return;
    if (p->xoff_from_dte || p->page_wait_active) return;
    if (p->remote_q_len == 0) return;
    n = p->remote_q_len;
    p->remote_q_len = 0;
    emit_remote_to_dte(p, p->remote_q, n);
}

/* --- X.29 PAD-message handling --------------------------------------- */

/* Send a PAD message body as a qualified data packet (qbit = 1). The
   message-type byte must already be in body[0]. Transports that lack
   Q-bit support (e.g. the Telnet bridge) return X25_ERR_NOT_SUPPORTED;
   we ignore that here - the local PAD did its job by trying. */
static int send_pad_msg(pad_session_t *p, const uint8 *body, uint32 len)
{
    if (!p->call.connected) return X25_ERR_CLEARED;
    return x25_send(&p->call, body, len, 1);
}

/* Apply an inbound X.29 PAD message. */
static void dispatch_x29_message(pad_session_t *p, const x29_message_t *m)
{
    uint8 i;

    switch (m->type) {

    case X29_MSG_PARAMETER_IND: {
        /* §4.5.4: remote PAD telling us a list of (ref, value) pairs.
           Surface them to the local DTE as a PAR service signal so the
           user sees what the remote PAD reported. */
        x28_param_pair_t pairs[X28_MAX_PARAMS];
        uint8 buf[256];
        int32 n;
        uint8 count = m->pair_count;
        if (count > X28_MAX_PARAMS) count = X28_MAX_PARAMS;
        if (!signals_enabled(p) || count == 0) break;
        for (i = 0; i < count; i++) {
            pairs[i].ref     = m->pairs[i].ref;
            pairs[i].value   = m->pairs[i].value;
            pairs[i].invalid = 0;
        }
        n = x28_format_par(pairs, count, buf, sizeof(buf));
        if (n > 0) to_dte(p, buf, (uint32)n);
        break;
    }

    case X29_MSG_INVITE_CLEAR:
        /* §4.5.3: remote asks us to clear. Treat like a remote-cleared
           call (DTE-originated, cause code 0). */
        pad_remote_cleared(p, PAD_CLR_REMOTE_REQUEST, 0, 0);
        break;

    case X29_MSG_SET:
        /* §4.5.1: remote sets our local X.3 parameters. No reply
           expected for plain Set; invalid pairs are silently skipped
           (a stricter implementation would respond with Error). */
        for (i = 0; i < m->pair_count; i++) {
            (void)x3_set(&p->params, m->pairs[i].ref, m->pairs[i].value);
        }
        break;

    case X29_MSG_READ: {
        /* §4.5.2: remote asks for our current values; reply with PI. */
        x29_pair_t resp[X29_MAX_PAIRS];
        uint8 count = 0;
        uint8 buf[1 + X29_MAX_PAIRS * 2];
        int32 n;
        for (i = 0; i < m->pair_count && count < X29_MAX_PAIRS; i++) {
            uint8 v = 0;
            if (x3_get(&p->params, m->pairs[i].ref, &v) == X3_OK) {
                resp[count].ref   = m->pairs[i].ref;
                resp[count].value = v;
                count++;
            }
        }
        if (count == 0) break;
        n = x29_encode_parameter_ind(resp, count, buf, sizeof(buf));
        if (n > 0) (void)send_pad_msg(p, buf, (uint32)n);
        break;
    }

    case X29_MSG_SET_READ: {
        /* §4.5.1 + §4.5.2: set then reply with PI of new values. */
        x29_pair_t resp[X29_MAX_PAIRS];
        uint8 count = 0;
        uint8 buf[1 + X29_MAX_PAIRS * 2];
        int32 n;
        for (i = 0; i < m->pair_count; i++) {
            (void)x3_set(&p->params, m->pairs[i].ref, m->pairs[i].value);
        }
        for (i = 0; i < m->pair_count && count < X29_MAX_PAIRS; i++) {
            uint8 v = 0;
            if (x3_get(&p->params, m->pairs[i].ref, &v) == X3_OK) {
                resp[count].ref   = m->pairs[i].ref;
                resp[count].value = v;
                count++;
            }
        }
        if (count == 0) break;
        n = x29_encode_parameter_ind(resp, count, buf, sizeof(buf));
        if (n > 0) (void)send_pad_msg(p, buf, (uint32)n);
        break;
    }

    case X29_MSG_BREAK:
        /* §4.5.5: remote DTE broke. If the message carries a new
           param 8 (discard output) value, apply it. Indicate the
           event to the local DTE the same way pad_remote_interrupted
           does (BEL when service signals are on). */
        if (m->break_has_param8) {
            p->params.values[X3_PAR_DISCARD] = m->break_param8;
        }
        if (p->state == PAD_STATE_DATA_TRANSFER && signals_enabled(p)) {
            uint8 bel = 0x07;
            to_dte(p, &bel, 1);
        }
        break;

    case X29_MSG_ERROR:
        /* §4.5.6: remote complained about something we sent. v1.2
           silently absorbs. A debug build could log error_code +
           error_msg_type for diagnostics. */
        break;

    case X29_MSG_RESELECTION:
        /* §4.5.7 reselection is out of scope; ignore. */
        break;

    case X29_MSG_UNKNOWN:
    default: {
        /* Reply with an Error PAD message: unrecognised type. */
        uint8 buf[8];
        int32 n = x29_encode_error(X29_ERR_UNRECOGNISED, 0,
                                   buf, sizeof(buf));
        if (n > 0) (void)send_pad_msg(p, buf, (uint32)n);
        break;
    }
    }
}

int pad_input_remote(pad_session_t *p, const uint8 *data, uint32 len,
                     uint8 qbit)
{
    if (p->trace_cb != NULL && len > 0) {
        p->trace_cb(p->trace_ctx, PAD_TRACE_REMOTE, data, len);
    }
    if (qbit) {
        /* X.29 qualified data packet (PAD message). Decode and
           dispatch. The decoder may reject the message; in that case
           emit an Error PAD message back per X.29 §4.5.6. */
        x29_message_t msg;
        if (x29_decode(data, len, &msg) == 0) {
            dispatch_x29_message(p, &msg);
        } else {
            uint8 ebuf[8];
            int32 n = x29_encode_error(X29_ERR_UNRECOGNISED,
                                       len > 0 ? data[0] : 0,
                                       ebuf, sizeof(ebuf));
            if (n > 0 && p->call.connected) {
                (void)x25_send(&p->call, ebuf, (uint32)n, 1);
            }
        }
        return 0;
    }
    if (p->state != PAD_STATE_DATA_TRANSFER ||
        p->xoff_from_dte || p->page_wait_active) {
        /* X.28 §4.3 / §4.14 / §4.18: bytes destined for the DTE while
           we're not in data transfer state OR while X-OFF / page wait is
           in effect are held until the gate opens. */
        queue_remote(p, data, len);
        return 0;
    }
    emit_remote_to_dte(p, data, len);
    return 0;
}

/* Apply X.3 param 7 break actions. The escape_allowed flag is 1 for a
   real break signal (X.28 4.11) and 0 for the BREAK PAD command signal
   (X.28 5.1) which never escapes from data transfer. */
static int do_break_actions(pad_session_t *p, int escape_allowed)
{
    uint8 brk = p->params.values[X3_PAR_BREAK];
    if (brk == 0) return 0;

    if (brk & 0x01) {
        if (p->call.connected) x25_interrupt(&p->call, 0);
    }
    if (brk & 0x02) {
        if (p->call.connected) x25_reset(&p->call, 0, 0);
    }
    /* Bit 2 (value 4): emit X.29 Indication-of-break PAD message
       (§4.5.5). Carries the current param 8 (discard output) value
       so the remote can sync its discard state, per the common
       pair-form interpretation. Bytes hit x25_send with qbit = 1; the
       Telnet bridge returns X25_ERR_NOT_SUPPORTED, which we ignore. */
    if ((brk & 0x04) && p->call.connected) {
        uint8 buf[3];
        int32 n;
        n = x29_encode_break(1, p->params.values[X3_PAR_DISCARD],
                             buf, sizeof(buf));
        if (n > 0) (void)x25_send(&p->call, buf, (uint32)n, 1);
    }
    if (escape_allowed && (brk & 0x08)) {
        if (p->state == PAD_STATE_DATA_TRANSFER) {
            p->state = PAD_STATE_WAITING_FOR_CMD;
        }
    }
    if (brk & 0x10) {
        p->params.values[X3_PAR_DISCARD] = 1;
    }
    return 0;
}

int pad_break(pad_session_t *p)
{
    return do_break_actions(p, 1);
}

int pad_tick(pad_session_t *p, uint32 elapsed_20ths)
{
    uint8 idle;
    if (p->state != PAD_STATE_DATA_TRANSFER) return 0;
    if (p->asm_len == 0) return 0;
    /* X.3 3.15 / X.28 §4.17: when editing is enabled in data transfer
       state, forwarding on idle-timer expiry is suspended. */
    if (p->params.values[X3_PAR_EDIT] == 1) return 0;
    idle = p->params.values[X3_PAR_IDLE];
    if (idle == 0) return 0;
    p->idle_ticks += elapsed_20ths;
    if (p->idle_ticks >= (uint32)idle) {
        flush_assembly(p);
        p->idle_ticks = 0;
    }
    return 0;
}
