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

/* ITU-T X.29 PAD-message encoder/decoder. Pure wire-format helpers
   with no PAD-state dependency. The PAD core (src/pad.c) calls these
   when bytes arrive on a Q-bit-set packet or when a local command
   needs to emit a PAD message. */
#include "x29.h"
#include <string.h>

/* ------------------------------------------------------------------------- */
/* Decoder                                                                   */
/* ------------------------------------------------------------------------- */

int x29_decode(const uint8 *buf, uint32 len, x29_message_t *out)
{
    uint32 i;
    uint8  type;

    memset(out, 0, sizeof(*out));
    out->type = X29_MSG_UNKNOWN;
    if (len == 0) return -1;
    type = buf[0];

    switch (type) {
    case X29_MSG_INVITE_CLEAR:
        /* §4.5.3: body is empty. Any trailing bytes are nonconformant
           but we accept them silently. */
        out->type = X29_MSG_INVITE_CLEAR;
        return 0;

    case X29_MSG_PARAMETER_IND:
    case X29_MSG_SET:
    case X29_MSG_SET_READ: {
        /* §4.5.1 / §4.5.4: (ref, value) pairs follow the type byte. */
        uint32 body = len - 1;
        if (body & 1) return -1;                /* odd-length body */
        if (body / 2 > X29_MAX_PAIRS) return -1;
        out->type = (x29_msg_type_t)type;
        out->pair_count = (uint8)(body / 2);
        for (i = 0; i < out->pair_count; i++) {
            out->pairs[i].ref   = buf[1 + i * 2];
            out->pairs[i].value = buf[1 + i * 2 + 1];
        }
        return 0;
    }

    case X29_MSG_READ: {
        /* §4.5.2: list of refs (single octets). */
        uint32 body = len - 1;
        if (body > X29_MAX_PAIRS) return -1;
        out->type = X29_MSG_READ;
        out->pair_count = (uint8)body;
        for (i = 0; i < out->pair_count; i++) {
            out->pairs[i].ref   = buf[1 + i];
            out->pairs[i].value = 0;
        }
        return 0;
    }

    case X29_MSG_BREAK: {
        /* §4.5.5: empty body, or a single octet pair {0x08, value}
           updating param 8 (discard output). Implementations vary;
           we accept (a) no body, (b) one byte interpreted as param 8
           value, (c) the pair form. */
        uint32 body = len - 1;
        out->type = X29_MSG_BREAK;
        if (body == 0) {
            out->break_has_param8 = 0;
        } else if (body == 1) {
            out->break_has_param8 = 1;
            out->break_param8     = buf[1];
        } else if (body == 2 && buf[1] == 8) {
            out->break_has_param8 = 1;
            out->break_param8     = buf[2];
        } else {
            return -1;
        }
        return 0;
    }

    case X29_MSG_ERROR: {
        /* §4.5.6 / Table 5: error code + offending message type byte.
           Optional further diagnostic octets are ignored. */
        if (len < 3) return -1;
        out->type           = X29_MSG_ERROR;
        out->error_code     = buf[1];
        out->error_msg_type = buf[2];
        return 0;
    }

    case X29_MSG_RESELECTION:
        /* §4.5.7: defer. We decode the type so the dispatcher can log
           "received reselection (out of scope)" without misreading
           the byte as one of the implemented messages. */
        out->type = X29_MSG_RESELECTION;
        return 0;

    default:
        /* Unknown message type; surface it so the PAD layer can emit
           an Error response. */
        out->type = X29_MSG_UNKNOWN;
        return -1;
    }
}

/* ------------------------------------------------------------------------- */
/* Encoders                                                                  */
/* ------------------------------------------------------------------------- */

static int32 encode_pairs(uint8 type_byte,
                          const x29_pair_t *pairs, uint8 count,
                          uint8 *buf, uint32 buf_size)
{
    uint32 need = 1u + (uint32)count * 2u;
    uint32 i;
    if (count > X29_MAX_PAIRS) return -1;
    if (need > buf_size)       return -1;
    buf[0] = type_byte;
    for (i = 0; i < count; i++) {
        buf[1 + i * 2]     = pairs[i].ref;
        buf[1 + i * 2 + 1] = pairs[i].value;
    }
    return (int32)need;
}

int32 x29_encode_parameter_ind(const x29_pair_t *pairs, uint8 count,
                               uint8 *buf, uint32 buf_size)
{
    return encode_pairs((uint8)X29_MSG_PARAMETER_IND,
                        pairs, count, buf, buf_size);
}

int32 x29_encode_set(const x29_pair_t *pairs, uint8 count,
                     uint8 *buf, uint32 buf_size)
{
    return encode_pairs((uint8)X29_MSG_SET, pairs, count, buf, buf_size);
}

int32 x29_encode_set_read(const x29_pair_t *pairs, uint8 count,
                          uint8 *buf, uint32 buf_size)
{
    return encode_pairs((uint8)X29_MSG_SET_READ,
                        pairs, count, buf, buf_size);
}

int32 x29_encode_invite_clear(uint8 *buf, uint32 buf_size)
{
    if (buf_size < 1) return -1;
    buf[0] = (uint8)X29_MSG_INVITE_CLEAR;
    return 1;
}

int32 x29_encode_read(const uint8 *refs, uint8 count,
                      uint8 *buf, uint32 buf_size)
{
    uint32 need = 1u + (uint32)count;
    uint32 i;
    if (count > X29_MAX_PAIRS) return -1;
    if (need > buf_size)       return -1;
    buf[0] = (uint8)X29_MSG_READ;
    for (i = 0; i < count; i++) buf[1 + i] = refs[i];
    return (int32)need;
}

int32 x29_encode_break(uint8 has_param8, uint8 param8_value,
                       uint8 *buf, uint32 buf_size)
{
    if (has_param8) {
        /* Use the explicit pair form so older PADs that key on a
           {0x08, v} pair recognise the message; modern PADs accept
           the single-octet form as well. */
        if (buf_size < 3) return -1;
        buf[0] = (uint8)X29_MSG_BREAK;
        buf[1] = 8;
        buf[2] = param8_value;
        return 3;
    }
    if (buf_size < 1) return -1;
    buf[0] = (uint8)X29_MSG_BREAK;
    return 1;
}

int32 x29_encode_error(uint8 error_code, uint8 offending_msg_type,
                       uint8 *buf, uint32 buf_size)
{
    if (buf_size < 3) return -1;
    buf[0] = (uint8)X29_MSG_ERROR;
    buf[1] = error_code;
    buf[2] = offending_msg_type;
    return 3;
}
