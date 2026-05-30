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

/* Write a 3-byte IAC verb-option sequence. */
static void write_iac3(int fd, uint8 verb, uint8 option)
{
    uint8 buf[3];
    if (fd < 0) return;
    buf[0] = TELNET_IAC;
    buf[1] = verb;
    buf[2] = option;
    (void)!write(fd, buf, 3);
}

/* Policy: which options do WE want to enable (drives our WILL/WONT)? */
static int policy_us(uint8 option)
{
    return (option == OPT_ECHO || option == OPT_BINARY || option == OPT_SGA);
}

/* Policy: which options do we want the PEER to enable (drives our DO/DONT)? */
static int policy_him(uint8 option)
{
    return (option == OPT_BINARY || option == OPT_SGA || option == OPT_NAWS);
}

/* Q-method: send WILL only if we are not already in YES or WANTYES.
   See RFC 1143 §"The Q Method of Implementing TELNET Option
   Negotiation": this gate is what prevents a stateless peer from
   driving us into an infinite negotiation loop. */
static void send_will(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) return;
    if (t->us[option] == Q_YES || t->us[option] == Q_WANTYES) return;
    t->us[option] = Q_WANTYES;
    write_iac3(t->fd, TELNET_WILL, option);
}

static void send_do(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) return;
    if (t->him[option] == Q_YES || t->him[option] == Q_WANTYES) return;
    t->him[option] = Q_WANTYES;
    write_iac3(t->fd, TELNET_DO, option);
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
    send_will(t, OPT_ECHO);
    send_will(t, OPT_SGA);
    send_do  (t, OPT_SGA);
    send_will(t, OPT_BINARY);
    send_do  (t, OPT_BINARY);
    send_do  (t, OPT_NAWS);
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

/* Q-method state transitions for incoming WILL/WONT/DO/DONT.
   The cardinal rule: when an incoming verb is the acknowledgement
   of a WILL/DO we already sent (state == Q_WANTYES), we silently
   transition to Q_YES with NO further reply. That's what kills the
   ping-pong loop a stateless peer would otherwise sustain. */
static void recv_will(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) {
        write_iac3(t->fd, TELNET_DONT, option);
        return;
    }
    switch (t->him[option]) {
    case Q_NO:
        if (policy_him(option)) {
            t->him[option] = Q_YES;
            write_iac3(t->fd, TELNET_DO, option);
        } else {
            write_iac3(t->fd, TELNET_DONT, option);
        }
        break;
    case Q_YES:
        break;
    case Q_WANTYES:
        t->him[option] = Q_YES;
        break;
    case Q_WANTNO:
        t->him[option] = Q_NO;
        break;
    }
}

static void recv_wont(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) return;
    switch (t->him[option]) {
    case Q_NO:
        break;
    case Q_YES:
        t->him[option] = Q_NO;
        write_iac3(t->fd, TELNET_DONT, option);
        break;
    case Q_WANTYES:
    case Q_WANTNO:
        t->him[option] = Q_NO;
        break;
    }
}

static void recv_do(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) {
        write_iac3(t->fd, TELNET_WONT, option);
        return;
    }
    switch (t->us[option]) {
    case Q_NO:
        if (policy_us(option)) {
            t->us[option] = Q_YES;
            write_iac3(t->fd, TELNET_WILL, option);
        } else {
            write_iac3(t->fd, TELNET_WONT, option);
        }
        break;
    case Q_YES:
        break;
    case Q_WANTYES:
        t->us[option] = Q_YES;
        break;
    case Q_WANTNO:
        t->us[option] = Q_NO;
        break;
    }
}

static void recv_dont(user_telnet_t *t, uint8 option)
{
    if (option >= UT_OPT_TABLE) return;
    switch (t->us[option]) {
    case Q_NO:
        break;
    case Q_YES:
        t->us[option] = Q_NO;
        write_iac3(t->fd, TELNET_WONT, option);
        break;
    case Q_WANTYES:
    case Q_WANTNO:
        t->us[option] = Q_NO;
        break;
    }
}

static void respond_to(user_telnet_t *t, uint8 verb, uint8 option)
{
    if      (verb == TELNET_WILL) recv_will (t, option);
    else if (verb == TELNET_WONT) recv_wont (t, option);
    else if (verb == TELNET_DO)   recv_do   (t, option);
    else if (verb == TELNET_DONT) recv_dont (t, option);
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
            } else if (t->last_was_cr && (b == 0x0A || b == 0x00)) {
                /* RFC 854 line-ending: CR LF and CR NUL are both
                   the network-virtual-terminal encoding of a bare
                   CR. Drop the trailing byte so the PAD core (which
                   uses CR alone as its command delimiter, X.28
                   §3.5.1) doesn't see the LF as a leading byte of
                   the next command. Standalone LF still passes
                   through so a user can send LF as data. */
                t->last_was_cr = 0;
            } else {
                out[j++] = b;
                t->last_was_cr = (b == 0x0D);
            }
            break;
        case UT_AFTER_IAC:
            if (b == TELNET_IAC) {
                out[j++] = TELNET_IAC; /* IAC IAC = literal 0xFF */
                t->last_was_cr = 0;
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
