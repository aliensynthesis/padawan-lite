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

/* Terminal-identity table.
 *
 * Maps a terminal type name to the bytes padawan-lite emits to a host
 * when asked "what kind of terminal are you?" via:
 *
 *   - Telnet TERMINAL-TYPE subnegotiation  (the name itself)
 *   - DEC ANSI Device Attributes 1 query   (ESC [ c, ESC [ 0 c)
 *   - VT52 Identify query                  (ESC Z, fallback used by
 *                                           some older hosts)
 *
 * The same source of truth feeds all three. An entry whose DA1
 * response is NULL means "don't auto-respond"; the host's query times
 * out and the host falls back to its dumb-terminal handling. Use
 * "DUMB" for that explicit behaviour.
 */

#ifndef PADAWAN_BRIDGE_TERM_ID_H
#define PADAWAN_BRIDGE_TERM_ID_H

#include "types.h"

typedef struct term_id_entry {
    const char  *name;           /* TTYPE-IS string, uppercase  */
    const uint8 *da1_response;   /* response to ESC [ c / ESC [ 0 c, NULL = none */
    uint32       da1_len;
    const uint8 *escz_response;  /* response to ESC Z, NULL = none */
    uint32       escz_len;
} term_id_entry_t;

/* Case-insensitive name lookup. Returns NULL if name is NULL, empty,
   or not in the table. */
const term_id_entry_t *term_id_lookup(const char *name);

/* Returns the default ("VT100") entry. Never NULL. */
const term_id_entry_t *term_id_default(void);

/* --- Inline DA-query interceptor ------------------------------------ */

/* Watches a host->user byte stream for the two ANSI device-identity
   queries hosts send and most users can't answer:
       ESC [ c     | ESC [ <digits/';'> c       (ECMA-48 / DEC DA1)
       ESC Z                                    (DEC VT52 Identify)
   When a query is detected the bytes are swallowed (not forwarded
   to the user terminal) and the appropriate response from the
   supplied term_id_entry_t is written into a separate response
   buffer (the caller writes it back to the host). All other escape
   sequences pass through unchanged. State carries across calls so
   a query split across two read()s is still recognised. */

typedef enum {
    TIF_NORMAL = 0,         /* no pending escape */
    TIF_AFTER_ESC,          /* saw ESC, awaiting next byte */
    TIF_AFTER_CSI,          /* saw ESC [, accumulating parameters */
    TIF_FLUSH_THROUGH       /* CSI overflow: pass bytes through until final */
} term_id_filter_state_t;

#define TERM_ID_FILTER_BUF 16

typedef struct term_id_filter {
    term_id_filter_state_t state;
    uint8                  buf[TERM_ID_FILTER_BUF];
    uint8                  buf_len;
} term_id_filter_t;

void term_id_filter_init(term_id_filter_t *t);

/* Process input bytes. Forwarded bytes go to out[] (out may equal in[]
   for in-place use). Auto-response bytes (the DA1 / ESC Z reply) are
   appended to response[] up to resp_max; *resp_len holds the bytes
   written this call (0 if no query fired). Returns the forwarded
   byte count. If id is NULL, term_id_default() is used. */
uint32 term_id_filter_process(term_id_filter_t       *t,
                              const term_id_entry_t  *id,
                              const uint8            *in,
                              uint32                  in_len,
                              uint8                  *out,
                              uint8                  *response,
                              uint32                  resp_max,
                              uint32                 *resp_len);

#endif
