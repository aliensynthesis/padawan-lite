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

/* PCP - PAD Control Protocol.

   A side-channel TCP listener on the bridge that lets a host
   application invoke X.29 PAD-message functions via a line-oriented
   text protocol on a SECOND TCP connection (separate from the user
   data connection). Solves the "Telnet has no Q-bit" problem
   without requiring the host to patch its Telnet library.

   See deviations.txt and memory/project_pcp.md for design background.

   Wire protocol (line-oriented, CRLF terminated, ASCII commands):

     host -> bridge:
       BIND <bridge-ip>:<bridge-port>      attach to a data session
       SET <ref>:<val>[,...]               X.29 Set
       READ <ref>[,...]                    X.29 Read
       SETREAD <ref>:<val>[,...]           X.29 Set-and-Read
       PAR <ref>:<val>[,...]               X.29 Parameter Indication
       ICLR                                X.29 Invitation to Clear
       BREAK [<param8-value>]              X.29 Indication of Break
       ERR <code>                          X.29 Error

     bridge -> host:
       OK [<info>]                         command accepted
       ERR <reason>                        command rejected
       EVT SET <ref>:<val>[,...]           inbound X.29 Set from PAD
       EVT READ <ref>[,...]                ... etc, mirror of above
       EVT SETREAD <ref>:<val>[,...]
       EVT PAR <ref>:<val>[,...]
       EVT ICLR
       EVT BREAK [<param8-value>]
       EVT ERR <code>

   Security:
     - Listener binds to 127.0.0.1 by default.
     - On BIND, the source IP of the PCP connection must match the
       peer IP of the data connection it's binding to.
     - At most one PCP connection may be bound to a given session
       concurrently; subsequent BIND attempts return ERR. */

#ifndef PADAWAN_BRIDGE_PCP_H
#define PADAWAN_BRIDGE_PCP_H

#include "types.h"
#include "pad.h"

#include <poll.h>

#define PCP_MAX_CONNS 16

/* Initialise PCP. port == 0 disables (the module is then a no-op).
   Returns 0 on success, -1 on listener-bind failure. */
int pcp_init(int port);

/* Tear down the listener and all active control connections. */
void pcp_shutdown(void);

/* True if PCP is enabled (pcp_init was called with a non-zero port). */
int pcp_enabled(void);

/* Listener fd, or -1 if PCP is off. */
int pcp_listener_fd(void);

/* Fill an array of pollfd-ready entries for every active PCP
   connection. Caller passes the entries[] array and its capacity;
   returns the number of entries written. Each entry's .events is
   set to POLLIN. */
int pcp_collect_pollfds(struct pollfd *out, int cap);

/* Driver callbacks (call from the main poll loop after a poll wakeup): */
void pcp_handle_accept(void);                    /* listener ready */
void pcp_handle_conn(int fd, short revents);    /* conn-fd ready */

/* Called by the bridge x25_send path when qbit = 1. If a PCP
   connection is bound to the session that owns this call, the X.29
   bytes are decoded and emitted as an EVT line on that connection;
   returns 0 (success). Returns -1 if no PCP connection is bound
   (caller falls back to X25_ERR_NOT_SUPPORTED). */
int pcp_emit_x29_event(const pad_session_t *p,
                       const uint8 *body, uint32 len);

/* Called by the bridge when a session is torn down so PCP can
   unbind any control connection attached to it (the connection
   stays open but is no longer bound; subsequent commands return
   ERR until the host issues a new BIND). */
void pcp_unbind_session(const pad_session_t *p);

#endif
