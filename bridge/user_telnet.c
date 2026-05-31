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

/* User-side Telnet IAC state machine, expressed as a thin server-
   role policy layer over the Telos protocol engine.

   What lives here vs. what lives in Telos:
     - Telos: IAC parser, Q-method state for all 256 options, NVT
       line-end normalisation, subnegotiation framing, send-side
       IAC escaping.
     - This file: policy callback (which options we accept WILL/DO
       for), NAWS subneg body decoding, the in/out buffer bridge for
       the existing user_telnet_filter() API. */

#define _POSIX_C_SOURCE 200809L

#include "user_telnet.h"

#include <string.h>
#include <unistd.h>

/* --- server-side policy ---------------------------------------------- */

/* Which options do WE agree to do (our WILL <opt> on peer's DO)? For
   the user-facing PAD: ECHO (we echo), BINARY (8-bit clean), SGA
   (char-at-a-time). */
static int policy_us(uint8 option)
{
    return (option == TELOS_OPT_ECHO    ||
            option == TELOS_OPT_BINARY  ||
            option == TELOS_OPT_SGA);
}

/* Which options do we want the PEER to do (our DO <opt> on peer's
   WILL)? BINARY, SGA, NAWS (client-to-server). ECHO from a client
   is refused because the PAD echoes for the client. */
static int policy_him(uint8 option)
{
    return (option == TELOS_OPT_BINARY ||
            option == TELOS_OPT_SGA    ||
            option == TELOS_OPT_NAWS);
}

static int policy_cb(void *ctx, uint8 option, telos_direction_t dir)
{
    (void)ctx;
    if (dir == TELOS_DIR_LOCAL)  return policy_us (option);
    if (dir == TELOS_DIR_REMOTE) return policy_him(option);
    return 0;
}

/* --- event handling -------------------------------------------------- */

static void event_cb(void *ctx, const telos_event_t *ev)
{
    user_telnet_t *t = (user_telnet_t *)ctx;
    switch (ev->type) {
    case TELOS_EV_DATA:
        /* Accumulate into the caller's filter buffer. The transient
           filter_out_* fields are set up by user_telnet_filter() for
           the duration of one telos_recv() call. */
        if (t->filter_out_buf != NULL && ev->u.data.len > 0) {
            uint32 room = (t->filter_out_cap > t->filter_out_len)
                          ? t->filter_out_cap - t->filter_out_len : 0;
            uint32 n    = (ev->u.data.len < room) ? ev->u.data.len : room;
            if (n > 0) {
                memcpy(t->filter_out_buf + t->filter_out_len,
                       ev->u.data.bytes, n);
                t->filter_out_len += n;
            }
        }
        break;
    case TELOS_EV_SUBNEG:
        /* RFC 1073: NAWS SB body is exactly four bytes:
             width_hi width_lo height_hi height_lo
           Longer bodies are tolerated (we use the first 4 bytes);
           shorter bodies are ignored. */
        if (ev->u.subneg.option == TELOS_OPT_NAWS &&
            ev->u.subneg.body_len >= 4) {
            const uint8 *b = ev->u.subneg.body;
            t->naws_width  = (uint16)(((uint16)b[0] << 8) | b[1]);
            t->naws_height = (uint16)(((uint16)b[2] << 8) | b[3]);
            t->has_naws    = 1;
        }
        break;
    default:
        /* COMMAND, OPTION_ENABLED/DISABLED, PROTO_ERROR not consumed
           here; the user-side PAD has no use for them today. */
        break;
    }
}

/* --- output to socket ------------------------------------------------ */

static void write_cb(void *ctx, const uint8 *bytes, uint32 len)
{
    user_telnet_t *t = (user_telnet_t *)ctx;
    if (t->fd >= 0 && len > 0) {
        (void)!write(t->fd, bytes, len);
    }
}

/* --- public API ------------------------------------------------------ */

void user_telnet_init(user_telnet_t *t, int fd)
{
    memset(t, 0, sizeof(*t));
    t->fd = fd;
    telos_init(&t->telos,
               TELOS_ROLE_SERVER,
               TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, t);
}

void user_telnet_send_initial(user_telnet_t *t)
{
    /* As a telnet server for the user's client we want:
         WILL ECHO    - we echo (client should disable local echo)
         WILL SGA     - char-at-a-time, no GA from us
         DO   SGA     - char-at-a-time, no GA from client
         WILL BINARY  - 8-bit clean output
         DO   BINARY  - 8-bit clean input
         DO   NAWS    - tell us your window size if you can. */
    telos_offer_will(&t->telos, TELOS_OPT_ECHO);
    telos_offer_will(&t->telos, TELOS_OPT_SGA);
    telos_offer_do  (&t->telos, TELOS_OPT_SGA);
    telos_offer_will(&t->telos, TELOS_OPT_BINARY);
    telos_offer_do  (&t->telos, TELOS_OPT_BINARY);
    telos_offer_do  (&t->telos, TELOS_OPT_NAWS);
}

uint32 user_telnet_filter(user_telnet_t *t,
                          const uint8 *in, uint32 in_len, uint8 *out)
{
    uint32 written;
    t->filter_out_buf = out;
    t->filter_out_len = 0;
    t->filter_out_cap = in_len;  /* Out can never exceed in_len. */

    telos_recv(&t->telos, in, in_len);

    written = t->filter_out_len;
    t->filter_out_buf = NULL;
    t->filter_out_len = 0;
    t->filter_out_cap = 0;
    return written;
}

int user_telnet_get_naws(const user_telnet_t *t,
                         uint16 *width, uint16 *height)
{
    if (!t->has_naws) return 0;
    if (width)  *width  = t->naws_width;
    if (height) *height = t->naws_height;
    return 1;
}

void user_telnet_write(int fd, const uint8 *data, uint32 len)
{
    /* Stateless helper: write `data` to `fd` with RFC 854 IAC
       escaping (any 0xFF in the data is doubled). This does NOT do
       NVT line-end encoding because callers emit explicit CR LF
       sequences as part of PAD signal text (X.28 §3) where the LF
       must travel verbatim, not be regenerated by an encoder. */
    uint8  buf[2048];
    uint32 i;
    uint32 j = 0;
    if (fd < 0 || len == 0) return;
    for (i = 0; i < len; i++) {
        if (j + 2 > sizeof(buf)) {
            (void)!write(fd, buf, j);
            j = 0;
        }
        buf[j++] = data[i];
        if (data[i] == 0xFF) buf[j++] = 0xFF;
    }
    if (j > 0) (void)!write(fd, buf, j);
}
