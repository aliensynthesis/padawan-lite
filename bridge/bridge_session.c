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

/* Front-end dispatch for the bridge. See bridge/bridge_session.h. */

#include <string.h>

#include "bridge_session.h"

bridge_session_t bridge_session_none(void)
{
    bridge_session_t s;
    memset(&s, 0, sizeof(s));
    s.kind = BRIDGE_SESSION_NONE;
    return s;
}

bridge_session_t bridge_session_from_pad(pad_session_t *p)
{
    bridge_session_t s = bridge_session_none();
    if (p != NULL) {
        s.kind   = BRIDGE_SESSION_PAD;
        s.as.pad = p;
    }
    return s;
}

bridge_session_t bridge_session_from_tymsat(tymsat_session_t *t)
{
    bridge_session_t s = bridge_session_none();
    if (t != NULL) {
        s.kind      = BRIDGE_SESSION_TYMSAT;
        s.as.tymsat = t;
    }
    return s;
}

int bridge_session_valid(const bridge_session_t *s)
{
    if (s == NULL) return 0;
    switch (s->kind) {
    case BRIDGE_SESSION_PAD:    return s->as.pad != NULL;
    case BRIDGE_SESSION_TYMSAT: return s->as.tymsat != NULL;
    case BRIDGE_SESSION_NONE:
    default:                    return 0;
    }
}

int bridge_session_same(const bridge_session_t *a, const bridge_session_t *b)
{
    if (!bridge_session_valid(a) || !bridge_session_valid(b)) return 0;
    if (a->kind != b->kind) return 0;
    switch (a->kind) {
    case BRIDGE_SESSION_PAD:    return a->as.pad == b->as.pad;
    case BRIDGE_SESSION_TYMSAT: return a->as.tymsat == b->as.tymsat;
    case BRIDGE_SESSION_NONE:
    default:                    return 0;
    }
}

pad_session_t *bridge_session_as_pad(const bridge_session_t *s)
{
    if (s == NULL || s->kind != BRIDGE_SESSION_PAD) return NULL;
    return s->as.pad;
}

void bridge_session_input_remote(const bridge_session_t *s,
                                 const uint8 *data, uint32 len)
{
    if (!bridge_session_valid(s) || data == NULL || len == 0) return;
    switch (s->kind) {
    case BRIDGE_SESSION_PAD:
        /* qbit 0: the Telnet transport cannot carry qualified data,
           so everything arriving is normal user data. */
        pad_input_remote(s->as.pad, data, len, 0);
        break;
    case BRIDGE_SESSION_TYMSAT:
        tymsat_input_remote(s->as.tymsat, data, len);
        break;
    case BRIDGE_SESSION_NONE:
    default:
        break;
    }
}

void bridge_session_call_connected(const bridge_session_t *s)
{
    if (!bridge_session_valid(s)) return;
    switch (s->kind) {
    case BRIDGE_SESSION_PAD:
        pad_call_connected(s->as.pad);
        break;
    case BRIDGE_SESSION_TYMSAT:
        tymsat_circuit_connected(s->as.tymsat);
        break;
    case BRIDGE_SESSION_NONE:
    default:
        break;
    }
}

/* Translate a transport cause into X.25 terms for the PAD. Reproduces
   the mapping the bridge applied inline before this module existed:
   X.25 §5.6.6 cause codes, diagnostic = raw errno. */
static void cleared_pad(pad_session_t *p, bridge_clear_cause_t cause,
                        uint8 diag)
{
    switch (cause) {
    case BRIDGE_CLEAR_REMOTE_CLOSED:
        pad_remote_cleared(p, PAD_CLR_REMOTE_REQUEST, 0, 0);
        break;
    case BRIDGE_CLEAR_REFUSED:
        pad_remote_cleared(p, PAD_CLR_NUMBER_NOT_ASSIGNED, 13, diag);
        break;
    case BRIDGE_CLEAR_UNREACHABLE:
        pad_remote_cleared(p, PAD_CLR_NUMBER_OUT_OF_ORDER, 9, diag);
        break;
    case BRIDGE_CLEAR_TIMEOUT:
    case BRIDGE_CLEAR_NETWORK_ERROR:
    default:
        pad_remote_cleared(p, PAD_CLR_NETWORK_PROBLEM, 5, diag);
        break;
    }
}

/* Translate a transport cause into the TYMNET message catalogue.

   Sourced from the message descriptions in "How to Use TYMNET" (July
   1982), lines 196-289:

     REMOTE_CLOSED -> DROPPED BY HOST SYSTEM [219]: "You have logged
       off and/or have been disconnected by the host."
     REFUSED       -> HOST DOWN [228]: "The network is fully
       operational, but the host computer is down." The connection
       reached the network's stand-in for the host and was declined,
       which is the host being down rather than the number being wrong
       -- BAD HOST NUMBER [204] belongs to an unresolvable number and
       is raised earlier, before any socket is opened.
     UNREACHABLE   -> HOST NOT AVAILABLE THRU NET [234]: "the TYMCOM,
       its neighbor(s), or a line between them is down".
     TIMEOUT       -> HOST NOT RESPONDING [237]: "an appropriate
       response to the connect request is not being received".
     NETWORK_ERROR -> TEMPORARY NETWORK PROBLEM [279]: "The network is
       currently unable to process traffic."

   The X.25 diagnostic byte has no counterpart in TYMNET's user-facing
   vocabulary and is dropped; see deviations.txt. */
static void cleared_tymsat(tymsat_session_t *t, bridge_clear_cause_t cause)
{
    tymsat_msg_t msg;

    switch (cause) {
    case BRIDGE_CLEAR_REMOTE_CLOSED:
        msg = TYMSAT_MSG_DROPPED_BY_HOST_SYSTEM;
        break;
    case BRIDGE_CLEAR_REFUSED:
        msg = TYMSAT_MSG_HOST_DOWN;
        break;
    case BRIDGE_CLEAR_UNREACHABLE:
        msg = TYMSAT_MSG_HOST_NOT_AVAILABLE;
        break;
    case BRIDGE_CLEAR_TIMEOUT:
        msg = TYMSAT_MSG_HOST_NOT_RESPONDING;
        break;
    case BRIDGE_CLEAR_NETWORK_ERROR:
    default:
        msg = TYMSAT_MSG_TEMPORARY_NETWORK_PROBLEM;
        break;
    }

    /* A failure during setup and a loss mid-session take different
       paths: the first re-prompts without a "disconnected" narrative,
       the second reports the loss first. tymsat_circuit_failed is a
       no-op outside CIRCUIT_BUILD and tymsat_circuit_cleared is a
       no-op outside DATA_TRANSFER/CIRCUIT_BUILD, so calling the right
       one by state keeps both honest. */
    if (t->state == TYMSAT_STATE_CIRCUIT_BUILD) {
        tymsat_circuit_failed(t, msg);
    } else {
        tymsat_circuit_cleared(t, msg);
    }
}

void bridge_session_cleared(const bridge_session_t *s,
                            bridge_clear_cause_t cause,
                            uint8 diag)
{
    if (!bridge_session_valid(s)) return;
    switch (s->kind) {
    case BRIDGE_SESSION_PAD:
        cleared_pad(s->as.pad, cause, diag);
        break;
    case BRIDGE_SESSION_TYMSAT:
        cleared_tymsat(s->as.tymsat, cause);
        break;
    case BRIDGE_SESSION_NONE:
    default:
        break;
    }
}

const char *bridge_session_terminal_type(const bridge_session_t *s)
{
    if (s == NULL || s->kind != BRIDGE_SESSION_PAD) return NULL;
    if (s->as.pad->terminal_type[0] == '\0') return NULL;
    return s->as.pad->terminal_type;
}
