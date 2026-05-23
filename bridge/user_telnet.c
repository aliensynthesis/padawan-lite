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

/* User-side Telnet IAC state machine. See bridge/user_telnet.h. */

#define _POSIX_C_SOURCE 200809L

#include "user_telnet.h"

#include <string.h>
#include <unistd.h>

#define TELNET_IAC   0xFF
#define TELNET_DONT  0xFE
#define TELNET_DO    0xFD
#define TELNET_WONT  0xFC
#define TELNET_WILL  0xFB
#define TELNET_SB    0xFA
#define TELNET_SE    0xF0
#define OPT_BINARY    0
#define OPT_ECHO      1
#define OPT_SGA       3
#define OPT_NAWS     31

void user_telnet_init(user_telnet_t *t, int fd)
{
    memset(t, 0, sizeof(*t));
    t->fd    = fd;
    t->state = UT_NORMAL;
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
    static const uint8 initial[] = {
        TELNET_IAC, TELNET_WILL, OPT_ECHO,
        TELNET_IAC, TELNET_WILL, OPT_SGA,
        TELNET_IAC, TELNET_DO,   OPT_SGA,
        TELNET_IAC, TELNET_WILL, OPT_BINARY,
        TELNET_IAC, TELNET_DO,   OPT_BINARY,
        TELNET_IAC, TELNET_DO,   OPT_NAWS
    };
    if (t->fd >= 0) {
        (void)!write(t->fd, initial, sizeof(initial));
    }
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
        if (data[i] == TELNET_IAC) buf[j++] = TELNET_IAC;
    }
    if (j > 0) (void)!write(fd, buf, j);
}

/* Respond to a verb+option from the user (client). Policy:
     User WILL X (client offering)
        -> DO    for BINARY, SGA, NAWS
        -> DONT  otherwise (including ECHO - the client must NOT echo)
     User DO X   (client asking us to do)
        -> WILL  for ECHO, BINARY, SGA
        -> WONT  otherwise (including NAWS - NAWS is client->server only)
     User WONT/DONT - no response. */
static void respond_to(user_telnet_t *t, uint8 verb, uint8 option)
{
    uint8 reply[3];
    reply[0] = TELNET_IAC;
    reply[2] = option;
    if (verb == TELNET_WILL) {
        if (option == OPT_BINARY || option == OPT_SGA || option == OPT_NAWS) {
            reply[1] = TELNET_DO;
        } else {
            reply[1] = TELNET_DONT;
        }
    } else if (verb == TELNET_DO) {
        if (option == OPT_ECHO || option == OPT_BINARY ||
            option == OPT_SGA) {
            reply[1] = TELNET_WILL;
        } else {
            reply[1] = TELNET_WONT;
        }
    } else {
        return;
    }
    if (t->fd >= 0) {
        (void)!write(t->fd, reply, sizeof(reply));
    }
}

/* When SE closes a sub-negotiation, see if it was NAWS and decode. */
static void finish_sb(user_telnet_t *t)
{
    if (t->sb_option == OPT_NAWS && t->sb_len >= 4) {
        t->naws_width  = (uint16)((t->sb_buf[0] << 8) | t->sb_buf[1]);
        t->naws_height = (uint16)((t->sb_buf[2] << 8) | t->sb_buf[3]);
        t->has_naws    = 1;
    }
    /* Other options ignored. */
}

uint32 user_telnet_filter(user_telnet_t *t,
                          const uint8 *in, uint32 in_len, uint8 *out)
{
    uint32 i, j = 0;
    for (i = 0; i < in_len; i++) {
        uint8 b = in[i];
        switch (t->state) {
        case UT_NORMAL:
            if (b == TELNET_IAC) {
                t->state = UT_AFTER_IAC;
            } else {
                out[j++] = b;
            }
            break;
        case UT_AFTER_IAC:
            if (b == TELNET_IAC) {
                out[j++] = TELNET_IAC; /* IAC IAC = literal 0xFF */
                t->state = UT_NORMAL;
            } else if (b == TELNET_WILL || b == TELNET_WONT ||
                       b == TELNET_DO   || b == TELNET_DONT) {
                t->verb  = b;
                t->state = UT_AFTER_VERB;
            } else if (b == TELNET_SB) {
                t->sb_option = 0;
                t->sb_len    = 0;
                t->state     = UT_IN_SB;
            } else {
                t->state = UT_NORMAL;
            }
            break;
        case UT_AFTER_VERB:
            respond_to(t, t->verb, b);
            t->state = UT_NORMAL;
            break;
        case UT_IN_SB:
            if (b == TELNET_IAC) {
                t->state = UT_IN_SB_AFTER_IAC;
            } else if (t->sb_len == 0) {
                t->sb_option = b;
                t->sb_len++;
            } else if (t->sb_len < sizeof(t->sb_buf) + 1) {
                /* sb_len counts option byte + payload; payload starts
                   at sb_buf[0] when sb_len == 1, ... */
                uint8 idx = (uint8)(t->sb_len - 1);
                if (idx < sizeof(t->sb_buf)) {
                    t->sb_buf[idx] = b;
                }
                t->sb_len++;
            }
            break;
        case UT_IN_SB_AFTER_IAC:
            if (b == TELNET_SE) {
                /* sb_len includes the option byte and counts the
                   payload bytes via the index math above; convert
                   "total bytes seen" to "payload length". */
                if (t->sb_len > 1) t->sb_len = (uint8)(t->sb_len - 1);
                else               t->sb_len = 0;
                finish_sb(t);
                t->state = UT_NORMAL;
            } else {
                t->state = UT_IN_SB;
            }
            break;
        }
    }
    return j;
}
