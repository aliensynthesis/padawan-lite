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

/* User-side Telnet IAC handling for pad-bridge --listen mode.

   In --listen mode pad-bridge is the telnet SERVER from the user's
   perspective. This module:
     - Sends initial option negotiation on connect.
     - Filters IAC sequences out of incoming user bytes before they
       reach the PAD command parser.
     - Responds to user-initiated negotiation requests with a policy
       suited to PAD use (server-side echo, char-at-a-time, 8-bit
       clean).
     - Captures user-reported window size (NAWS SB) for later use.

   This is conceptually parallel to the IAC code inside
   x25_telnet_bridge.c, but the roles are reversed (we are server
   here, client there) so the policies differ. The state machine
   structure is the same; minor duplication accepted at v1.2. */

#ifndef PADAWAN_BRIDGE_USER_TELNET_H
#define PADAWAN_BRIDGE_USER_TELNET_H

#include "types.h"

typedef enum {
    UT_NORMAL = 0,
    UT_AFTER_IAC,
    UT_AFTER_VERB,
    UT_IN_SB,
    UT_IN_SB_AFTER_IAC
} user_telnet_iac_t;

/* RFC 1143 Q-method per-option agreement states. NO/YES are the
   stable agreed states; WANTYES/WANTNO are the "request pending"
   states between a WILL/DO and its DO/WILL acknowledgement. We
   track WANTYES_OPPOSITE-style sub-states implicitly via Q_WANTNO
   because padawan-lite never reverses its mind mid-negotiation. */
typedef enum {
    Q_NO       = 0,
    Q_YES      = 1,
    Q_WANTYES  = 2,
    Q_WANTNO   = 3
} user_telnet_q_t;

/* Option index range we maintain Q-state for: options 0..31. This
   covers BINARY (0), ECHO (1), SGA (3), TTYPE (24), NAWS (31).
   Requests for options outside this range get a stateless refusal
   (DONT/WONT) to keep memory bounded. */
#define UT_OPT_TABLE 32

typedef struct {
    int               fd;
    user_telnet_iac_t state;
    uint8             verb;
    uint8             sb_option;
    uint8             sb_buf[8];
    uint8             sb_len;
    int               has_naws;
    uint16            naws_width;
    uint16            naws_height;
    user_telnet_q_t   us[UT_OPT_TABLE];   /* per-option: are WE doing it? */
    user_telnet_q_t   him[UT_OPT_TABLE];  /* per-option: is PEER doing it? */
    /* RFC 854 line-ending normalisation state. Set when the most
       recent emitted data byte was CR; on the next byte, an LF or
       NUL is recognised as the CR LF / CR NUL sequence and dropped
       so the PAD core sees a bare CR (X.28 §3.5.1 command
       delimiter is CR alone). Carries across read() boundaries. */
    int               last_was_cr;
} user_telnet_t;

/* Reset state and remember the user socket fd. */
void user_telnet_init(user_telnet_t *t, int fd);

/* Send the bridge's initial option block (WILL ECHO/SGA/BINARY,
   DO SGA/BINARY/NAWS) on the user socket. Safe to call once after
   accept. */
void user_telnet_send_initial(user_telnet_t *t);

/* Run incoming bytes through the IAC state machine. Returns the
   number of "data" bytes written to out (always <= in_len). May
   write negotiation responses to t->fd as a side effect. */
uint32 user_telnet_filter(user_telnet_t *t,
                          const uint8 *in, uint32 in_len, uint8 *out);

/* If the user reported a window size via NAWS, return 1 and fill
   width/height; otherwise return 0. */
int user_telnet_get_naws(const user_telnet_t *t,
                         uint16 *width, uint16 *height);

/* Write PAD-to-user data with RFC 854 IAC escaping: a literal 0xFF
   data byte is sent as IAC IAC. Use this from the bridge driver for
   bytes going to a telnet-client user socket so a real telnet client
   doesn't misinterpret them. fd should be a connected TCP fd. */
void user_telnet_write(int fd, const uint8 *data, uint32 len);

#endif
