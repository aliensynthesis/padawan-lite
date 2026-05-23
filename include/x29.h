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

/* ITU-T X.29 (12/97) - PAD-message format and procedures.
   PAD messages flow over X.25 qualified data packets (Q-bit = 1). This
   module is the wire-format encode/decode layer only; semantic
   dispatch (apply a Set, respond to a Read, react to an Invitation to
   clear, etc.) is done by the PAD core in src/pad.c, which calls
   these functions when it sees qbit-marked data on the way in or
   needs to emit a PAD message on the way out.

   The message-type byte is the first octet of the qualified data
   packet body (Table 1/X.29). Subsequent octets are message-specific
   per X.29 clause 4. */
#ifndef PADAWAN_X29_H
#define PADAWAN_X29_H

#include "types.h"

/* X.29 Table 1: PAD message type codes (first octet of a qualified
   data packet). */
typedef enum {
    X29_MSG_PARAMETER_IND = 0x00,   /* §4.5.4: PI - parameter values */
    X29_MSG_INVITE_CLEAR  = 0x01,   /* §4.5.3: ICLR - invitation to clear */
    X29_MSG_SET           = 0x02,   /* §4.5.1: Set parameters */
    X29_MSG_BREAK         = 0x03,   /* §4.5.5: Indication of break */
    X29_MSG_READ          = 0x04,   /* §4.5.2: Read parameters */
    X29_MSG_ERROR         = 0x05,   /* §4.5.6: Error response */
    X29_MSG_SET_READ      = 0x06,   /* §4.5.1/2: Set then read */
    X29_MSG_RESELECTION   = 0x07,   /* §4.5.7: Reselection (out of scope) */
    X29_MSG_UNKNOWN       = 0xFF    /* decoded type unrecognised */
} x29_msg_type_t;

/* X.29 Error PAD message error-code field (§4.5.6 / Table 5). */
#define X29_ERR_UNRECOGNISED   0x00  /* unrecognised PAD message */
#define X29_ERR_BAD_PARAMETER  0x01  /* unsupported reference */
#define X29_ERR_BAD_VALUE      0x02  /* invalid value */
#define X29_ERR_READ_ONLY      0x03  /* attempt to set a read-only ref */
#define X29_ERR_BAD_LENGTH     0x04  /* invalid PAD-message length */
#define X29_ERR_BAD_FORMAT     0x05  /* invalid PAD-message format */

#define X29_MAX_PAIRS    32   /* per Set/Read/SetRead/PI message */

typedef struct {
    uint8 ref;
    uint8 value;
} x29_pair_t;

typedef struct {
    x29_msg_type_t type;
    uint8          pair_count;
    x29_pair_t     pairs[X29_MAX_PAIRS]; /* PI/Set/SetRead use ref+value;
                                            Read uses ref only (.value=0) */
    /* For Indication of break (§4.5.5): the break PAD message can be
       empty or carry a Q-flagged "set param 8" pair. break_has_param8
       is 1 when the message carries an updated param-8 value;
       break_param8 is that value. */
    uint8          break_has_param8;
    uint8          break_param8;
    /* For Error message: error_code and the message-type byte of the
       offending message (X.29 §4.5.6 / Table 5). */
    uint8          error_code;
    uint8          error_msg_type;
} x29_message_t;

/* Decode bytes from a single qualified data packet body into out.
   Returns 0 on success, < 0 on a malformed message. The caller is
   expected to have already verified that these bytes came from a
   Q-bit-set packet (qbit = 1). */
int x29_decode(const uint8 *buf, uint32 len, x29_message_t *out);

/* PAD message formatters. Each returns the number of bytes written
   into buf, or a negative value on overflow. The encoded form starts
   with the X29_MSG_* type byte and is suitable for handing to
   x25_send with qbit = 1. */

/* §4.5.4 PI - Parameter indication. Used as the reply to a Read or a
   Set-and-read, and also to surface the values a PAD chose during
   setup. */
int32 x29_encode_parameter_ind(const x29_pair_t *pairs, uint8 count,
                               uint8 *buf, uint32 buf_size);

/* §4.5.3 ICLR - Invitation to clear. Body is empty (just the type byte). */
int32 x29_encode_invite_clear(uint8 *buf, uint32 buf_size);

/* §4.5.1 Set - apply (ref, value) pairs at the remote PAD. */
int32 x29_encode_set(const x29_pair_t *pairs, uint8 count,
                     uint8 *buf, uint32 buf_size);

/* §4.5.5 Indication of break. has_param8 = 0 means "break only";
   has_param8 = 1 includes the post-break param 8 value (1 = discard
   set, 0 = clear) so the remote can sync its discard state with the
   PAD that detected the break. */
int32 x29_encode_break(uint8 has_param8, uint8 param8_value,
                       uint8 *buf, uint32 buf_size);

/* §4.5.2 Read - request a list of refs from the remote PAD. */
int32 x29_encode_read(const uint8 *refs, uint8 count,
                      uint8 *buf, uint32 buf_size);

/* §4.5.6 Error - error_code per X29_ERR_* and the type byte of the
   message that triggered the error. */
int32 x29_encode_error(uint8 error_code, uint8 offending_msg_type,
                       uint8 *buf, uint32 buf_size);

/* §4.5.1 + §4.5.2 Set and read - apply (ref, value) pairs and expect
   the remote to reply with a Parameter indication containing the new
   values. */
int32 x29_encode_set_read(const x29_pair_t *pairs, uint8 count,
                          uint8 *buf, uint32 buf_size);

#endif
