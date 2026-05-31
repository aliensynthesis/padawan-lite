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

/* Terminal-identity table. See term_id.h for the rationale. */

#include "term_id.h"

#include <stddef.h>
#include <string.h>

/* DA1 responses per DEC VT-series manuals (and ECMA-48 §5.4 "Device
   Control Strings" / DEC's DA reply form). Each begins with ESC [ ?
   and is terminated by 'c'.

   VT100   "VT100 series, no options"
   VT102   "DEC VT102"
   VT220   "VT220 series, options 1 (132 columns), 2 (printer),
            6 (selective erase), 8 (UDKs), 9 (NRC sets), 15 (DEC tech)"
   XTERM   matches xterm's well-known DA1: "VT100 with AVO" (i.e.
            advanced video option set). Many hosts treat this as a
            full VT100 driver. */
static const uint8 RESP_VT100_DA1[] = { 0x1B, '[', '?', '1', ';', '0', 'c' };
static const uint8 RESP_VT102_DA1[] = { 0x1B, '[', '?', '6', 'c' };
static const uint8 RESP_VT220_DA1[] = {
    0x1B, '[', '?', '6', '2', ';',
    '1', ';', '2', ';', '6', ';', '8', ';', '9', ';', '1', '5', 'c'
};
static const uint8 RESP_XTERM_DA1[] = { 0x1B, '[', '?', '1', ';', '2', 'c' };

/* VT52 Identify response, per the VT52 manual: ESC / Z.
   Used by VT100/VT102 in VT52 compatibility mode too. Modern
   terminals (VT220 onward) don't respond to ESC Z. */
static const uint8 RESP_VT52_ESCZ[] = { 0x1B, '/', 'Z' };

static const term_id_entry_t TABLE[] = {
    { "VT52",  NULL,            0,
                                  RESP_VT52_ESCZ, sizeof(RESP_VT52_ESCZ) },
    { "VT100", RESP_VT100_DA1,  sizeof(RESP_VT100_DA1),
                                  RESP_VT52_ESCZ, sizeof(RESP_VT52_ESCZ) },
    { "VT102", RESP_VT102_DA1,  sizeof(RESP_VT102_DA1),
                                  RESP_VT52_ESCZ, sizeof(RESP_VT52_ESCZ) },
    { "VT220", RESP_VT220_DA1,  sizeof(RESP_VT220_DA1),
                                  NULL,           0 },
    { "XTERM", RESP_XTERM_DA1,  sizeof(RESP_XTERM_DA1),
                                  NULL,           0 },
    { "DUMB",  NULL,            0,
                                  NULL,           0 },
    { NULL,    NULL,            0,
                                  NULL,           0 }
};

static int eq_icase(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

const term_id_entry_t *term_id_lookup(const char *name)
{
    int i;
    if (name == NULL || *name == '\0') return NULL;
    for (i = 0; TABLE[i].name != NULL; i++) {
        if (eq_icase(TABLE[i].name, name)) return &TABLE[i];
    }
    return NULL;
}

const term_id_entry_t *term_id_default(void)
{
    return term_id_lookup("VT100");
}

/* --- inline DA-query interceptor implementation --------------------- */

void term_id_filter_init(term_id_filter_t *t)
{
    t->state   = TIF_NORMAL;
    t->buf_len = 0;
}

/* Copy the buffered escape-prefix bytes to out[] starting at j; clear
   the buffer; return the new j. Safe with out aliased to the same
   array the bytes were sourced from (the bytes match what's already
   at out[i_at_buffer_start..i-1], so it's an identity write). */
static uint32 flush_buf(term_id_filter_t *t, uint8 *out, uint32 j)
{
    if (t->buf_len > 0) {
        memmove(out + j, t->buf, t->buf_len);
        j += t->buf_len;
        t->buf_len = 0;
    }
    return j;
}

/* Append src[0..src_len-1] to response[dst_pos..]; cap at dst_max.
   Drops the response entirely if it would not fit (so a degenerate
   storm of queries doesn't truncate one mid-reply). */
static uint32 append_response(const uint8 *src, uint32 src_len,
                              uint8 *dst, uint32 dst_pos, uint32 dst_max)
{
    if (src == NULL || src_len == 0) return dst_pos;
    if (dst_pos + src_len > dst_max) return dst_pos;
    memcpy(dst + dst_pos, src, src_len);
    return dst_pos + src_len;
}

uint32 term_id_filter_process(term_id_filter_t       *t,
                              const term_id_entry_t  *id,
                              const uint8            *in,
                              uint32                  in_len,
                              uint8                  *out,
                              uint8                  *response,
                              uint32                  resp_max,
                              uint32                 *resp_len)
{
    uint32 i;
    uint32 j  = 0;
    uint32 rj = 0;
    uint8  b;

    if (id == NULL) id = term_id_default();
    if (resp_len != NULL) *resp_len = 0;

    for (i = 0; i < in_len; i++) {
        b = in[i];
        switch (t->state) {
        case TIF_NORMAL:
            if (b == 0x1B) {
                t->buf[0]  = b;
                t->buf_len = 1;
                t->state   = TIF_AFTER_ESC;
            } else {
                out[j++] = b;
            }
            break;
        case TIF_AFTER_ESC:
            if (b == '[') {
                if (t->buf_len < TERM_ID_FILTER_BUF) {
                    t->buf[t->buf_len++] = b;
                }
                t->state = TIF_AFTER_CSI;
            } else if (b == 'Z') {
                /* VT52 Identify: swallow ESC Z, emit reply. */
                rj = append_response(id->escz_response, id->escz_len,
                                     response, rj, resp_max);
                t->buf_len = 0;
                t->state   = TIF_NORMAL;
            } else {
                /* Some other 2-byte escape (or start of a longer one
                   we don't intercept) -- flush and pass through. */
                j = flush_buf(t, out, j);
                out[j++] = b;
                t->state = TIF_NORMAL;
            }
            break;
        case TIF_AFTER_CSI:
            if ((b >= '0' && b <= '9') || b == ';') {
                if (t->buf_len < TERM_ID_FILTER_BUF) {
                    t->buf[t->buf_len++] = b;
                } else {
                    j = flush_buf(t, out, j);
                    out[j++] = b;
                    t->state = TIF_FLUSH_THROUGH;
                }
            } else if (b == 'c') {
                /* DA1 query: swallow, emit reply. */
                rj = append_response(id->da1_response, id->da1_len,
                                     response, rj, resp_max);
                t->buf_len = 0;
                t->state   = TIF_NORMAL;
            } else {
                /* Some other CSI final (H/J/K/m/...) or intermediate
                   we don't recognise: flush prefix and the byte. */
                j = flush_buf(t, out, j);
                out[j++] = b;
                t->state = TIF_NORMAL;
            }
            break;
        case TIF_FLUSH_THROUGH:
            out[j++] = b;
            if (b >= 0x40 && b <= 0x7E) {
                t->state = TIF_NORMAL;
            }
            break;
        }
    }

    if (resp_len != NULL) *resp_len = rj;
    return j;
}
