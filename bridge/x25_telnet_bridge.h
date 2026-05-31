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

/* Telnet bridge - driver↔bridge contract.

   This header is the contract between the bridge's driver (bridge/main.c)
   and the bridge's x25_telnet_bridge.c. It is NOT part of Padawan-Lite's
   abstract X.25 API (include/x25.h); the driver needs poll() integration
   helpers that don't belong in the generic interface.

   The bridge depends only on Padawan-Lite's public headers (include/x25.h,
   include/pad.h, include/types.h). Moving this entire directory to a
   separate project requires no code changes - only an outer Makefile
   that links libpadawancore.a and points -I at padawan's include/. */
#ifndef PADAWAN_BRIDGE_X25_TELNET_BRIDGE_H
#define PADAWAN_BRIDGE_X25_TELNET_BRIDGE_H

#include "pad.h"

/* Bind the PAD session that receives bridge-originated events
   (pad_input_remote, pad_call_connected, pad_remote_cleared, ...).
   v1.2 is single-session; multi-session would track per-call_id. */
void x25_bridge_bind(pad_session_t *p);

/* Returns the socket fd of the active call, or -1 if no call is up.
   The driver includes this fd in its poll() set. */
int x25_bridge_get_fd(void);

/* Driver callback: invoke when poll() reports activity on the bridge fd.
   Handles connect completion, data delivery to pad_input_remote, and
   peer-close / error -> pad_remote_cleared. */
void x25_bridge_poll_events(short revents);

/* Load an address->host:port mapping table from a config file. File
   format: one entry per line, "<address> <host> <port>". Lines starting
   with '#' and blank lines are ignored. Returns 0 on success, -1 on
   open error. Existing map is replaced. v1.2 capacity is small but
   sufficient for typical use; see deviations.txt. */
int x25_bridge_load_map(const char *filename);

/* Update the advertised terminal window size (RFC 1073 NAWS). The size
   is stored; if a call is active AND the server has negotiated NAWS,
   an updated SB NAWS is sent immediately. Default size when never set
   is 80x24. The driver typically calls this once at startup with
   TIOCGWINSZ and again from a SIGWINCH handler. */
void x25_bridge_set_window_size(uint16 width, uint16 height);

/* Set the default terminal type the bridge claims to hosts (via
   TELNET TTYPE subnegotiation and inline DEC DA1 / VT52 Identify
   auto-responses) when the user has not supplied a non-empty
   response at the Telenet "TERMINAL=" prompt. Accepts the names
   defined in bridge/term_id.c (vt52, vt100, vt102, vt220, xterm,
   dumb, unknown, ansi), case-insensitive. Returns 0 on success, -1
   if the name
   is not in the table. Per-session user input always wins over
   this default; see effective_ttype_name(). Calling with NULL or
   an empty string is treated as "unset" (revert to built-in
   "vt100" fallback). */
int x25_bridge_set_ttype_claim(const char *name);

/* --- multi-session helpers ------------------------------------------- */

/* Return the socket fd of the active call bound to the given session,
   or -1 if that session has no active call. Used by multi-session
   drivers to build their poll() set. */
int x25_bridge_fd_for_session(const pad_session_t *p);

/* Like x25_bridge_set_window_size but only updates the call bound to
   the specified session. Use this from multi-user drivers that get a
   NAWS update for one user and don't want to overwrite other users'
   sizes. No-op if the session has no active call. */
void x25_bridge_set_window_size_for_session(const pad_session_t *p,
                                            uint16 width, uint16 height);

/* Per-fd variant of x25_bridge_poll_events. Locates the bridge call
   slot whose fd matches and dispatches events to it (connect-complete,
   data delivery, peer-close). For single-session drivers, the simpler
   x25_bridge_poll_events still works. */
void x25_bridge_poll_fd(int fd, short revents);

/* --- PCP integration accessors --------------------------------------- */

/* Look up the active session whose bridge-side outbound TCP has the
   given local source IP and port. Used by PCP for BIND command
   resolution: the host already knows the source endpoint of the
   bridge's data connection (from its own accept()) and sends it as
   the BIND argument. Returns NULL if no matching call is active.
   ip_str is dotted-quad IPv4 (e.g. "192.168.1.5"); port is host
   byte order. */
pad_session_t *x25_bridge_session_at_local(const char *ip_str, int port);

/* Write the printable peer IP of the bridge's outbound TCP for the
   given session into ip_out (size ip_out_sz, at least 16 bytes for
   INET_ADDRSTRLEN). Returns 0 on success, -1 if no active call.
   Used by PCP to validate that a binding control connection comes
   from the same host as the data connection. */
int x25_bridge_peer_ip_for_session(const pad_session_t *p,
                                   char *ip_out, uint32 ip_out_sz);

#endif
