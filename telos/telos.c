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

/* Telos — Telnet protocol engine. See telos.h for design summary. */

#include "telos.h"

#include <string.h>     /* memset, memcpy */
#include <stddef.h>     /* NULL */

#define IAC  TELOS_CMD_IAC
#define WILL TELOS_CMD_WILL
#define WONT TELOS_CMD_WONT
#define DO   TELOS_CMD_DO
#define DONT TELOS_CMD_DONT
#define SB   TELOS_CMD_SB
#define SE   TELOS_CMD_SE

#define IS_SINGLE_BYTE_CMD(c) \
    ((c) >= TELOS_CMD_NOP && (c) <= TELOS_CMD_GA)

/* === Low-level helpers ============================================== */

static void emit_event(telos_session_t *s, const telos_event_t *ev)
{
    if (s->event_cb != NULL) s->event_cb(s->ctx, ev);
}

static void emit_write(telos_session_t *s, const uint8 *bytes, uint32 len)
{
    if (s->write_cb != NULL && len > 0) s->write_cb(s->ctx, bytes, len);
}

static void emit_iac2(telos_session_t *s, uint8 cmd)
{
    uint8 b[2];
    b[0] = IAC;
    b[1] = cmd;
    emit_write(s, b, 2);
}

static void emit_iac3(telos_session_t *s, uint8 verb, uint8 option)
{
    uint8 b[3];
    b[0] = IAC;
    b[1] = verb;
    b[2] = option;
    emit_write(s, b, 3);
}

static void emit_option_event(telos_session_t  *s,
                              telos_event_type_t type,
                              uint8              option,
                              telos_direction_t  dir)
{
    telos_event_t ev;
    ev.type             = type;
    ev.u.option.option  = option;
    ev.u.option.direction = dir;
    emit_event(s, &ev);
}

static void emit_proto_error(telos_session_t *s,
                             const char      *reason,
                             uint8            byte)
{
    telos_event_t ev;
    if ((s->flags & TELOS_FLAG_STRICT_PEER) == 0) return;
    ev.type                 = TELOS_EV_PROTO_ERROR;
    ev.u.proto_error.reason = reason;
    ev.u.proto_error.byte   = byte;
    emit_event(s, &ev);
}

static int policy_says_yes(telos_session_t   *s,
                           uint8              option,
                           telos_direction_t  dir)
{
    if (s->policy_cb == NULL) return 0;
    return s->policy_cb(s->ctx, option, dir) ? 1 : 0;
}

/* === Q-method handlers (RFC 1143) =================================== */

/* Peer sent WILL <option>: peer wants to do option. Affects him[option]. */
static void recv_will(telos_session_t *s, uint8 option)
{
    switch (s->him[option]) {
    case TELOS_Q_NO:
        if (policy_says_yes(s, option, TELOS_DIR_REMOTE)) {
            s->him[option] = TELOS_Q_YES;
            emit_iac3(s, DO, option);
            emit_option_event(s, TELOS_EV_OPTION_ENABLED,
                              option, TELOS_DIR_REMOTE);
        } else {
            emit_iac3(s, DONT, option);
        }
        break;
    case TELOS_Q_YES:
        emit_proto_error(s, "redundant WILL on YES", option);
        break;
    case TELOS_Q_WANTYES:
        s->him[option] = TELOS_Q_YES;
        emit_option_event(s, TELOS_EV_OPTION_ENABLED,
                          option, TELOS_DIR_REMOTE);
        break;
    case TELOS_Q_WANTNO:
        /* We sent DONT, peer's WILL is stale. Per RFC 1143 the
           queue-aware reading is "we already moved to NO; ignore". */
        s->him[option] = TELOS_Q_NO;
        emit_proto_error(s, "WILL while WANTNO", option);
        break;
    }
}

/* Peer sent WONT <option>: peer refuses or withdraws. Affects him[option]. */
static void recv_wont(telos_session_t *s, uint8 option)
{
    switch (s->him[option]) {
    case TELOS_Q_NO:
        /* Already off; silent absorb. */
        break;
    case TELOS_Q_YES:
        s->him[option] = TELOS_Q_NO;
        emit_iac3(s, DONT, option);
        emit_option_event(s, TELOS_EV_OPTION_DISABLED,
                          option, TELOS_DIR_REMOTE);
        break;
    case TELOS_Q_WANTYES:
        /* Our DO was refused. */
        s->him[option] = TELOS_Q_NO;
        break;
    case TELOS_Q_WANTNO:
        /* Expected reply to our DONT. */
        s->him[option] = TELOS_Q_NO;
        emit_option_event(s, TELOS_EV_OPTION_DISABLED,
                          option, TELOS_DIR_REMOTE);
        break;
    }
}

/* Peer sent DO <option>: peer wants us to do option. Affects us[option]. */
static void recv_do(telos_session_t *s, uint8 option)
{
    switch (s->us[option]) {
    case TELOS_Q_NO:
        if (policy_says_yes(s, option, TELOS_DIR_LOCAL)) {
            s->us[option] = TELOS_Q_YES;
            emit_iac3(s, WILL, option);
            emit_option_event(s, TELOS_EV_OPTION_ENABLED,
                              option, TELOS_DIR_LOCAL);
        } else {
            emit_iac3(s, WONT, option);
        }
        break;
    case TELOS_Q_YES:
        emit_proto_error(s, "redundant DO on YES", option);
        break;
    case TELOS_Q_WANTYES:
        s->us[option] = TELOS_Q_YES;
        emit_option_event(s, TELOS_EV_OPTION_ENABLED,
                          option, TELOS_DIR_LOCAL);
        break;
    case TELOS_Q_WANTNO:
        s->us[option] = TELOS_Q_NO;
        emit_proto_error(s, "DO while WANTNO", option);
        break;
    }
}

/* Peer sent DONT <option>: peer refuses us doing it. Affects us[option]. */
static void recv_dont(telos_session_t *s, uint8 option)
{
    switch (s->us[option]) {
    case TELOS_Q_NO:
        break;
    case TELOS_Q_YES:
        s->us[option] = TELOS_Q_NO;
        emit_iac3(s, WONT, option);
        emit_option_event(s, TELOS_EV_OPTION_DISABLED,
                          option, TELOS_DIR_LOCAL);
        break;
    case TELOS_Q_WANTYES:
        s->us[option] = TELOS_Q_NO;
        break;
    case TELOS_Q_WANTNO:
        s->us[option] = TELOS_Q_NO;
        emit_option_event(s, TELOS_EV_OPTION_DISABLED,
                          option, TELOS_DIR_LOCAL);
        break;
    }
}

/* === Parser ========================================================= */

/* Flush accumulated data bytes via the event callback. */
static void flush_data(telos_session_t *s,
                       const uint8 *buf, uint32 len)
{
    telos_event_t ev;
    if (len == 0) return;
    ev.type         = TELOS_EV_DATA;
    ev.u.data.bytes = buf;
    ev.u.data.len   = len;
    emit_event(s, &ev);
}

/* Append a data byte to the output buffer, flushing if it would
   overflow. Used in NORMAL state. */
static void push_data_byte(telos_session_t *s,
                           uint8 b,
                           uint8 *outbuf, uint32 *outlen, uint32 outcap)
{
    if (*outlen >= outcap) {
        flush_data(s, outbuf, *outlen);
        *outlen = 0;
    }
    outbuf[(*outlen)++] = b;
}

/* In NORMAL state, deciding whether to consume an NVT line-ending
   byte (LF/NUL after CR) or push it as data. Returns 1 if consumed. */
static int nvt_should_consume(telos_session_t *s, uint8 b)
{
    if ((s->flags & TELOS_FLAG_NVT_LINE_ENDING) == 0) return 0;
    if (s->him[TELOS_OPT_BINARY] == TELOS_Q_YES)      return 0;
    if (!s->last_was_cr)                              return 0;
    if (b != 0x0A && b != 0x00)                       return 0;
    return 1;
}

void telos_recv(telos_session_t *s, const uint8 *bytes, uint32 len)
{
    uint8  outbuf[256];
    uint32 outlen = 0;
    uint32 i;

    for (i = 0; i < len; i++) {
        uint8 b = bytes[i];
        switch (s->state) {

        case TELOS_PS_NORMAL:
            if (b == IAC) {
                flush_data(s, outbuf, outlen);
                outlen       = 0;
                s->state     = TELOS_PS_AFTER_IAC;
            } else if (nvt_should_consume(s, b)) {
                /* Drop LF/NUL after CR per RFC 854. */
                s->last_was_cr = 0;
            } else {
                push_data_byte(s, b, outbuf, &outlen, sizeof(outbuf));
                s->last_was_cr = (b == 0x0D) ? 1 : 0;
            }
            break;

        case TELOS_PS_AFTER_IAC:
            if (b == IAC) {
                /* IAC IAC = literal 0xFF in data. */
                push_data_byte(s, IAC, outbuf, &outlen, sizeof(outbuf));
                s->last_was_cr = 0;
                s->state       = TELOS_PS_NORMAL;
            } else if (b == WILL || b == WONT || b == DO || b == DONT) {
                s->verb  = b;
                s->state = TELOS_PS_AFTER_VERB;
            } else if (b == SB) {
                s->sb_option = 0;
                s->sb_len    = 0;
                s->state     = TELOS_PS_IN_SB;
            } else if (IS_SINGLE_BYTE_CMD(b)) {
                telos_event_t ev;
                ev.type            = TELOS_EV_COMMAND;
                ev.u.command.cmd   = b;
                emit_event(s, &ev);
                s->state           = TELOS_PS_NORMAL;
            } else {
                /* Unrecognised byte after IAC. Strict reading: error.
                   Tolerant: ignore. */
                emit_proto_error(s, "unknown IAC verb", b);
                s->state = TELOS_PS_NORMAL;
            }
            break;

        case TELOS_PS_AFTER_VERB:
            if      (s->verb == WILL) recv_will (s, b);
            else if (s->verb == WONT) recv_wont (s, b);
            else if (s->verb == DO)   recv_do   (s, b);
            else if (s->verb == DONT) recv_dont (s, b);
            s->state = TELOS_PS_NORMAL;
            break;

        case TELOS_PS_IN_SB:
            if (b == IAC) {
                s->state = TELOS_PS_IN_SB_AFTER_IAC;
            } else if (s->sb_len == 0) {
                /* First byte after IAC SB is the option number. */
                s->sb_option = b;
                s->sb_len    = 1;   /* sb_len includes the option byte */
            } else {
                /* Body byte. Skip silently on overflow. */
                if (s->sb_len - 1 < TELOS_SB_MAX) {
                    s->sb_buf[s->sb_len - 1] = b;
                }
                s->sb_len++;
            }
            break;

        case TELOS_PS_IN_SB_AFTER_IAC:
            if (b == SE) {
                telos_event_t ev;
                uint32 body_len = (s->sb_len > 0) ? s->sb_len - 1 : 0;
                if (body_len > TELOS_SB_MAX) body_len = TELOS_SB_MAX;
                ev.type              = TELOS_EV_SUBNEG;
                ev.u.subneg.option   = s->sb_option;
                ev.u.subneg.body     = s->sb_buf;
                ev.u.subneg.body_len = body_len;
                emit_event(s, &ev);
                s->state = TELOS_PS_NORMAL;
            } else if (b == IAC) {
                /* IAC IAC inside SB = literal 0xFF body byte. */
                if (s->sb_len == 0) {
                    /* IAC IAC before the option byte? Treat as
                       protocol error; option byte stays 0. */
                    emit_proto_error(s, "IAC IAC before SB option", b);
                } else if (s->sb_len - 1 < TELOS_SB_MAX) {
                    s->sb_buf[s->sb_len - 1] = IAC;
                }
                s->sb_len++;
                s->state = TELOS_PS_IN_SB;
            } else {
                /* IAC followed by something other than IAC or SE
                   inside an SB: spec says abort the SB. */
                emit_proto_error(s, "unexpected IAC in SB", b);
                s->state = TELOS_PS_NORMAL;
            }
            break;
        }
    }

    flush_data(s, outbuf, outlen);
}

/* === Send-path encoders ============================================ */

void telos_send_data(telos_session_t *s, const uint8 *bytes, uint32 len)
{
    /* Walk the buffer, doubling any IAC byte, and emitting in
       contiguous chunks via the write callback to avoid per-byte
       calls. NVT CR encoding (CR -> CR LF) is applied when
       NVT_LINE_ENDING is requested and BINARY is not YES in the
       local direction. */
    int    nvt   = ((s->flags & TELOS_FLAG_NVT_LINE_ENDING) != 0) &&
                   (s->us[TELOS_OPT_BINARY] != TELOS_Q_YES);
    uint32 i;
    uint32 chunk_start = 0;
    static const uint8 IAC_IAC[2] = { IAC, IAC };
    static const uint8 CR_LF[2]   = { 0x0D, 0x0A };

    for (i = 0; i < len; i++) {
        uint8 b = bytes[i];
        int   special = 0;

        if (b == IAC)                        special = 1;
        else if (nvt && b == 0x0D)           special = 2;

        if (special != 0) {
            if (i > chunk_start) {
                emit_write(s, bytes + chunk_start, i - chunk_start);
            }
            if (special == 1) {
                emit_write(s, IAC_IAC, 2);
            } else {
                emit_write(s, CR_LF, 2);
            }
            chunk_start = i + 1;
        }
    }
    if (len > chunk_start) {
        emit_write(s, bytes + chunk_start, len - chunk_start);
    }
}

void telos_offer_will(telos_session_t *s, uint8 option)
{
    if (s->us[option] == TELOS_Q_YES || s->us[option] == TELOS_Q_WANTYES) return;
    s->us[option] = TELOS_Q_WANTYES;
    emit_iac3(s, WILL, option);
}

void telos_offer_do(telos_session_t *s, uint8 option)
{
    if (s->him[option] == TELOS_Q_YES || s->him[option] == TELOS_Q_WANTYES) return;
    s->him[option] = TELOS_Q_WANTYES;
    emit_iac3(s, DO, option);
}

void telos_withdraw_will(telos_session_t *s, uint8 option)
{
    if (s->us[option] == TELOS_Q_NO || s->us[option] == TELOS_Q_WANTNO) return;
    s->us[option] = TELOS_Q_WANTNO;
    emit_iac3(s, WONT, option);
}

void telos_withdraw_do(telos_session_t *s, uint8 option)
{
    if (s->him[option] == TELOS_Q_NO || s->him[option] == TELOS_Q_WANTNO) return;
    s->him[option] = TELOS_Q_WANTNO;
    emit_iac3(s, DONT, option);
}

void telos_send_subneg(telos_session_t *s, uint8 option,
                       const uint8 *body, uint32 body_len)
{
    /* Frame: IAC SB <option> body... IAC SE, doubling 0xFF in body. */
    uint8  hdr[3];
    uint8  trailer[2];
    uint32 i;
    uint32 chunk_start = 0;
    static const uint8 IAC_IAC[2] = { IAC, IAC };

    hdr[0]     = IAC;
    hdr[1]     = SB;
    hdr[2]     = option;
    trailer[0] = IAC;
    trailer[1] = SE;

    emit_write(s, hdr, 3);
    for (i = 0; i < body_len; i++) {
        if (body[i] == IAC) {
            if (i > chunk_start) {
                emit_write(s, body + chunk_start, i - chunk_start);
            }
            emit_write(s, IAC_IAC, 2);
            chunk_start = i + 1;
        }
    }
    if (body_len > chunk_start) {
        emit_write(s, body + chunk_start, body_len - chunk_start);
    }
    emit_write(s, trailer, 2);
}

void telos_send_command(telos_session_t *s, uint8 cmd)
{
    if (!IS_SINGLE_BYTE_CMD(cmd) && cmd != IAC) return;
    emit_iac2(s, cmd);
}

telos_q_t telos_q_state(const telos_session_t *s,
                        uint8 option, telos_direction_t direction)
{
    return (direction == TELOS_DIR_LOCAL) ? s->us[option] : s->him[option];
}

/* === Init =========================================================== */

void telos_init(telos_session_t *s,
                telos_role_t      role,
                uint32            flags,
                telos_policy_fn   policy_cb,
                telos_event_fn    event_cb,
                telos_write_fn    write_cb,
                void             *ctx)
{
    memset(s, 0, sizeof(*s));
    s->role      = role;
    s->flags     = flags;
    s->policy_cb = policy_cb;
    s->event_cb  = event_cb;
    s->write_cb  = write_cb;
    s->ctx       = ctx;
    /* All Q-states default to NO via memset; parser state TELOS_PS_NORMAL = 0. */
}
