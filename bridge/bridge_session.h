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

/* Front-end session handle for the bridge.

   Padawan-Lite has two user-facing front ends, and they are peers
   rather than layers:

     - the X.28 PAD          (include/pad.h, --emulate default/telenet/...)
     - the stand-alone TYMSAT (include/tymsat.h, --emulate tymnet)

   Both originate calls and both consume bytes arriving from the far
   end, but they share no vocabulary above that: the PAD speaks X.28
   service signals and X.29 qualified data, the TYMSAT speaks TYMNET
   login messages and has no packet layer at all. See include/tymsat.h
   for why the TYMSAT cannot be modelled as a PAD personality.

   The bridge does not care which it is driving -- it needs exactly
   three operations (deliver inbound bytes, report call established,
   report call gone) plus identity comparison. This header supplies a
   tagged handle carrying those, so x25_telnet_bridge.c stays free of
   front-end specifics.

   The bridge still depends only on Padawan-Lite's PUBLIC headers
   (include/pad.h, include/tymsat.h, include/x25.h, include/types.h),
   so bridge/ remains extraction-ready. */
#ifndef PADAWAN_BRIDGE_SESSION_H
#define PADAWAN_BRIDGE_SESSION_H

#include "pad.h"
#include "tymsat.h"

typedef enum {
    BRIDGE_SESSION_NONE   = 0,   /* unbound slot */
    BRIDGE_SESSION_PAD    = 1,
    BRIDGE_SESSION_TYMSAT = 2
} bridge_session_kind_t;

typedef struct {
    bridge_session_kind_t kind;
    union {
        pad_session_t    *pad;
        tymsat_session_t *tymsat;
    } as;
} bridge_session_t;

/* Transport-level clear causes.

   Deliberately expressed in the transport's own terms (what the socket
   did) rather than either front end's vocabulary, so the translation
   into X.25 cause codes or TYMNET messages happens in exactly one
   place. The PAD mapping reproduces what the bridge did before this
   type existed: X.25 §5.6.6 cause codes, diagnostic = raw errno. */
typedef enum {
    BRIDGE_CLEAR_REMOTE_CLOSED = 0,  /* clean FIN, or POLLHUP/POLLERR */
    BRIDGE_CLEAR_REFUSED,            /* ECONNREFUSED */
    BRIDGE_CLEAR_UNREACHABLE,        /* EHOSTUNREACH/ENETUNREACH/EHOSTDOWN */
    BRIDGE_CLEAR_TIMEOUT,            /* ETIMEDOUT */
    BRIDGE_CLEAR_NETWORK_ERROR       /* anything else, incl. mid-call errors */
} bridge_clear_cause_t;

/* Constructors. Passing NULL yields an unbound (BRIDGE_SESSION_NONE)
   handle rather than a half-initialised one. */
bridge_session_t bridge_session_from_pad(pad_session_t *p);
bridge_session_t bridge_session_from_tymsat(tymsat_session_t *t);
bridge_session_t bridge_session_none(void);

/* Non-zero when the handle refers to a live front end. */
int bridge_session_valid(const bridge_session_t *s);

/* Non-zero when both handles refer to the same front-end instance.
   Two unbound handles are NOT considered the same. */
int bridge_session_same(const bridge_session_t *a, const bridge_session_t *b);

/* The PAD behind the handle, or NULL when it is not a PAD session.
   For the few genuinely PAD-specific integrations -- notably PCP,
   which is an X.29 event channel and has no TYMSAT meaning. */
pad_session_t *bridge_session_as_pad(const bridge_session_t *s);

/* --- dispatch -------------------------------------------------------- */

/* Deliver bytes that arrived from the far end. */
void bridge_session_input_remote(const bridge_session_t *s,
                                 const uint8 *data, uint32 len);

/* Report that call setup completed. */
void bridge_session_call_connected(const bridge_session_t *s);

/* Report that the call is gone, translating cause into whichever
   vocabulary the bound front end speaks. diag carries the raw errno
   where one applies (0 otherwise); the PAD surfaces it as an X.25
   diagnostic byte, the TYMSAT has no field for it and drops it. */
void bridge_session_cleared(const bridge_session_t *s,
                            bridge_clear_cause_t cause,
                            uint8 diag);

/* The terminal type the user supplied at a front-end prompt, or NULL
   when there is none.

   Only the PAD captures this, via the Telenet personality's
   "TERMINAL=" prompt. The TYMSAT's terminal identifier is a different
   mechanism -- a single character selecting speed and character set
   rather than a terminal-model name (see include/tymsat.h) -- and is
   deliberately not offered here, so TYMSAT sessions fall through to
   the operator default set by --ttype-claim. */
const char *bridge_session_terminal_type(const bridge_session_t *s);

#endif
