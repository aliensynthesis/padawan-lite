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

/* X.25 abstract service implementation backed by TCP-to-telnet.

   Implements every function in include/x25.h:
     - x25_init / x25_call / x25_clear / x25_reset / x25_interrupt
     - x25_send / x25_recv
   x25_recv is a stub; data flows the other way - the driver calls
   x25_bridge_poll_events on socket readability and we push bytes via
   pad_input_remote into the session bound to the call.

   Address interpretation: an X.25 address is looked up in an optional
   address->host:port map loaded via x25_bridge_load_map(). Unmapped
   addresses fall back to "address-as-port on 127.0.0.1".

   Telnet handling:
     - On connect: send DO/WILL SGA, DO/WILL BINARY, DONT/WONT ECHO.
     - Respond to server's DO TERMINAL-TYPE (RFC 1091) and DO NAWS
       (RFC 1073). Other server-initiated options refused.
     - Inbound IAC sequences filtered; SB TERMINAL-TYPE SEND answered
       with "PADAWAN"; SB for anything else discarded.
     - Outbound 0xFF doubled per RFC 854.

   v1.2 multi-session: state is per-call in g_calls[BRIDGE_MAX_CALLS].
   The current bridge driver (bridge/main.c) is still single-session
   and uses the compat APIs (x25_bridge_get_fd / x25_bridge_poll_events)
   which operate on the first active slot. */

#define _POSIX_C_SOURCE 200809L

#include "x25_telnet_bridge.h"
#include "x25.h"
#include "pad.h"
#include "pcp.h"
#include "term_id.h"
#include "telos.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

/* --- per-call state ---------------------------------------------------- */

#define BRIDGE_MAX_CALLS 16

typedef struct {
    int            in_use;
    int            fd;
    int            connecting;
    pad_session_t *session;

    /* Generation counter, incremented on alloc. Encoded into the upper
       24 bits of x25_call_t.call_id so a stale handle pointing at a
       reused slot can be detected (call_slot returns NULL on mismatch). */
    uint32         generation;

    /* Embedded Telnet protocol engine in client role. As of v1.5.4
       all IAC parsing, Q-method state for all 256 options, NVT
       line-end normalisation, and subneg framing delegate here.
       The bridge keeps only host-facing application-level state
       (NAWS dimensions, TTYPE rotation index, ANSI DA interceptor). */
    telos_session_t telos;

    /* NAWS state (per-connection). */
    int            naws_active;
    uint16         naws_width;
    uint16         naws_height;

    /* RFC 1091 TERMINAL-TYPE rotation index: how many SB TTYPE IS
       responses we've sent on this connection. Telemetry only. */
    uint8          ttype_index;

    /* Inline DA query interceptor state. Watches host->user bytes
       for ESC [ c / ESC [ <param> c (DA1) and ESC Z (VT52 Identify)
       and auto-responds on behalf of the user's terminal. */
    term_id_filter_t term_id;
} bridge_call_t;

static bridge_call_t  g_calls[BRIDGE_MAX_CALLS];

/* Session bound to the NEXT x25_call. Multi-session callers would set
   this immediately before each x25_call to associate the new call with
   the right session. The single-session driver sets it once at startup. */
static pad_session_t *g_default_session = NULL;

/* Default window size used when allocating a new slot. Updated by
   x25_bridge_set_window_size. */
static uint16 g_default_naws_width  = 80;
static uint16 g_default_naws_height = 24;

/* Operator-set default terminal name (--ttype-claim). Empty means
   "no operator default; fall through to the term_id_default()
   entry." See effective_term_id() and x25_bridge_set_ttype_claim. */
#define BRIDGE_TTYPE_MAX 31
static char g_ttype_claim_default[BRIDGE_TTYPE_MAX + 1] = "";

/* --- address map (process-global) ------------------------------------- */

#define BRIDGE_MAP_MAX     32
#define BRIDGE_ADDR_MAX    16
#define BRIDGE_HOST_MAX    64

typedef struct {
    char           address[BRIDGE_ADDR_MAX];
    char           host[BRIDGE_HOST_MAX];
    unsigned short port;
} bridge_map_entry_t;

static bridge_map_entry_t g_map[BRIDGE_MAP_MAX];
static int                g_map_count = 0;

int x25_bridge_load_map(const char *filename)
{
    FILE *f;
    char  line[256];

    if (filename == NULL) return -1;
    f = fopen(filename, "r");
    if (f == NULL) return -1;

    g_map_count = 0;
    while (fgets(line, sizeof(line), f) != NULL &&
           g_map_count < BRIDGE_MAP_MAX) {
        char addr[BRIDGE_ADDR_MAX];
        char host[BRIDGE_HOST_MAX];
        int  port;
        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r') {
            continue;
        }
        if (sscanf(line, "%15s %63s %d", addr, host, &port) != 3) {
            continue;
        }
        if (port < 1 || port > 65535) continue;
        strcpy(g_map[g_map_count].address, addr);
        strcpy(g_map[g_map_count].host, host);
        g_map[g_map_count].port = (unsigned short)port;
        g_map_count++;
    }
    fclose(f);
    return 0;
}

static int map_lookup(const char *address, char *host_out, size_t host_max,
                      unsigned short *port_out)
{
    int i;
    for (i = 0; i < g_map_count; i++) {
        if (strcmp(g_map[i].address, address) == 0) {
            strncpy(host_out, g_map[i].host, host_max - 1);
            host_out[host_max - 1] = '\0';
            *port_out = g_map[i].port;
            return 1;
        }
    }
    return 0;
}

static int parse_port(const char *s)
{
    long v;
    char *end;
    if (s == NULL || *s == '\0') return -1;
    v = strtol(s, &end, 10);
    if (end == s || *end != '\0') return -1;
    if (v < 1 || v > 65535) return -1;
    return (int)v;
}

/* --- slot allocation -------------------------------------------------- */

/* Forward decls for the Telos callbacks installed at slot init time;
   the definitions live further down in the file alongside the rest
   of the Telnet handling. */
static int  bridge_policy_cb(void *ctx, uint8 option,
                             telos_direction_t dir);
static void bridge_event_cb (void *ctx, const telos_event_t *ev);
static void bridge_write_cb (void *ctx, const uint8 *bytes, uint32 len);

/* Encoding: low 8 bits = slot index (BRIDGE_MAX_CALLS is small),
   high 24 bits = generation. Generation rolls over after 16M reuses
   per slot, which is fine for any realistic deployment. */
#define CALL_ID_SLOT(id)  ((int)((id) & 0xFF))
#define CALL_ID_GEN(id)   ((uint32)(((uint32)(id)) >> 8))
#define MAKE_CALL_ID(idx, gen) \
    ((int32)((((uint32)(gen)) << 8) | ((uint32)(idx) & 0xFF)))

static int alloc_slot(uint32 *gen_out)
{
    int i;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (!g_calls[i].in_use) {
            uint32 next_gen = g_calls[i].generation + 1;
            memset(&g_calls[i], 0, sizeof(g_calls[i]));
            g_calls[i].in_use      = 1;
            g_calls[i].generation  = next_gen;
            g_calls[i].fd          = -1;
            g_calls[i].naws_width  = g_default_naws_width;
            g_calls[i].naws_height = g_default_naws_height;
            /* Initialise the embedded Telos session in client role.
               No TELOS_FLAG_NVT_LINE_ENDING here: the host->user
               byte stream feeds a real terminal, which needs the LF
               in CR LF kept intact to advance to the next line. The
               PAD-side user_telnet session does set the flag (PAD
               command parser at the @ prompt wants bare CR). */
            telos_init(&g_calls[i].telos,
                       TELOS_ROLE_CLIENT,
                       0,
                       bridge_policy_cb, bridge_event_cb, bridge_write_cb,
                       &g_calls[i]);
            if (gen_out) *gen_out = next_gen;
            return i;
        }
    }
    return -1;
}

static bridge_call_t *call_slot(const x25_call_t *call)
{
    int idx;
    uint32 gen;
    if (call == NULL) return NULL;
    idx = CALL_ID_SLOT(call->call_id);
    gen = CALL_ID_GEN(call->call_id);
    if (idx < 0 || idx >= BRIDGE_MAX_CALLS) return NULL;
    if (!g_calls[idx].in_use) return NULL;
    if (g_calls[idx].generation != gen) return NULL;
    return &g_calls[idx];
}

static int find_first_active(void)
{
    int i;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (g_calls[i].in_use && g_calls[i].fd >= 0) return i;
    }
    return -1;
}

static void slot_close(bridge_call_t *s)
{
    if (s == NULL) return;
    if (s->fd >= 0) {
        /* Half-close the write side first so the peer sees a clean
           TCP FIN before the socket goes away. Helps line-oriented
           hosts (e.g. SIMH's DZ device behind VMS) recognise the
           hangup promptly instead of holding the line until idle
           timeout. EAGAIN/ENOTCONN are expected on a half-open
           or already-failed socket; ignore. */
        (void)shutdown(s->fd, SHUT_WR);
        close(s->fd);
        s->fd = -1;
    }
    s->connecting = 0;
    s->naws_active = 0;
    s->ttype_index = 0;
    s->in_use     = 0;
    /* Telos state is left as-is; the next alloc_slot will memset and
       telos_init the slot afresh. */
    s->session    = NULL;
}

/* --- Telnet IAC handling ---------------------------------------------- */

#define TELNET_IAC   0xFF
#define TELNET_DONT  0xFE
#define TELNET_DO    0xFD
#define TELNET_WONT  0xFC
#define TELNET_WILL  0xFB
#define TELNET_SB    0xFA
#define TELNET_SE    0xF0
#define TELNET_OPT_BINARY  0
#define TELNET_OPT_ECHO    1
#define TELNET_OPT_SGA     3
#define TELNET_OPT_TTYPE   24
#define TELNET_OPT_NAWS    31
#define TELNET_TTYPE_IS    0
#define TELNET_TTYPE_SEND  1

/* Forward decls for the send helpers; bridge_event_cb invokes them
   in response to subneg / option events from Telos. */
static void send_naws_sb(bridge_call_t *s);
static void send_terminal_type(bridge_call_t *s);

/* Resolve the effective terminal identity for a bridge call. The
   precedence chain (highest first):
     1. Per-session: a non-empty s->session->terminal_type captured at
        the personality's TERMINAL= prompt (Telenet).
     2. Operator default: --ttype-claim NAME set via
        x25_bridge_set_ttype_claim().
     3. Built-in: term_id_default() (VT100).
   Always returns a non-NULL entry. If the user's TERMINAL= response
   names a terminal the table doesn't know, falls through as if it
   were absent -- safer than claiming bytes we can't produce. */
static const term_id_entry_t *effective_term_id(const bridge_call_t *s)
{
    const term_id_entry_t *e;
    if (s != NULL && s->session != NULL &&
        s->session->terminal_type[0] != '\0') {
        e = term_id_lookup(s->session->terminal_type);
        if (e != NULL) return e;
    }
    if (g_ttype_claim_default[0] != '\0') {
        e = term_id_lookup(g_ttype_claim_default);
        if (e != NULL) return e;
    }
    return term_id_default();
}

int x25_bridge_set_ttype_claim(const char *name)
{
    size_t n;
    if (name == NULL || *name == '\0') {
        g_ttype_claim_default[0] = '\0';
        return 0;
    }
    if (term_id_lookup(name) == NULL) return -1;
    n = strlen(name);
    if (n > BRIDGE_TTYPE_MAX) n = BRIDGE_TTYPE_MAX;
    memcpy(g_ttype_claim_default, name, n);
    g_ttype_claim_default[n] = '\0';
    return 0;
}

/* Telos policy: which options do WE agree to in each direction?
   Same answers as the pre-Telos policy_us_bridge / policy_him_bridge
   helpers, now expressed as a single direction-keyed callback. */
static int bridge_policy_cb(void *ctx, uint8 option, telos_direction_t dir)
{
    (void)ctx;
    if (dir == TELOS_DIR_LOCAL) {
        return (option == TELOS_OPT_BINARY ||
                option == TELOS_OPT_SGA    ||
                option == TELOS_OPT_TTYPE  ||
                option == TELOS_OPT_NAWS);
    }
    /* TELOS_DIR_REMOTE: do we want the HOST to do this option? */
    return (option == TELOS_OPT_BINARY ||
            option == TELOS_OPT_SGA);
}

/* Telos event dispatch for the host side. Three event types matter
   to us:
     - DATA: clean post-IAC host-to-PAD bytes. Run them through the
       ANSI DA query interceptor (term_id_filter), write any DA reply
       back to the host, then forward the survivors to pad_input_remote.
     - SUBNEG: handle the two RFC 1091 / RFC 1073 subnegs we care
       about. NAWS from the host is meaningless (host is the server;
       NAWS is client->server only). TTYPE SEND triggers our IS reply.
     - OPTION_ENABLED: when WE turn NAWS on (us[NAWS] = YES), push our
       current dimensions in a follow-up SB so the host sees them
       without us waiting for it to ask. */
static void bridge_event_cb(void *ctx, const telos_event_t *ev)
{
    bridge_call_t *s = (bridge_call_t *)ctx;

    switch (ev->type) {
    case TELOS_EV_DATA: {
        /* Telos delivers data events in chunks of at most its
           internal outbuf size (currently 256 bytes). We copy to a
           local buffer so term_id_filter_process can do its in-place
           transform without mutating Telos's internal buffer. */
        uint8  data[256];
        uint8  resp[64];
        uint32 dlen     = ev->u.data.len;
        uint32 resp_len = 0;
        uint32 filtered_len;
        if (dlen > sizeof(data)) dlen = sizeof(data);
        if (dlen == 0) break;
        memcpy(data, ev->u.data.bytes, dlen);
        filtered_len = term_id_filter_process(
            &s->term_id, effective_term_id(s),
            data, dlen, data,
            resp, sizeof(resp), &resp_len);
        if (resp_len > 0 && s->fd >= 0) {
            (void)!write(s->fd, resp, resp_len);
        }
        if (filtered_len > 0 && s->session != NULL) {
            /* User-data flow from the Telnet peer is always qbit=0;
               true X.29 needs a real X.25 transport. */
            pad_input_remote(s->session, data, filtered_len, 0);
        }
        break;
    }

    case TELOS_EV_SUBNEG:
        if (ev->u.subneg.option == TELOS_OPT_TTYPE &&
            ev->u.subneg.body_len >= 1 &&
            ev->u.subneg.body[0] == TELNET_TTYPE_SEND) {
            send_terminal_type(s);
        }
        /* SB NAWS from host (RFC 1073): NAWS is documented as a
           client->server option; we are the client, so a host
           sending us its window size is out of scope. Silently
           ignore. */
        break;

    case TELOS_EV_OPTION_ENABLED:
        if (ev->u.option.option == TELOS_OPT_NAWS &&
            ev->u.option.direction == TELOS_DIR_LOCAL) {
            s->naws_active = 1;
            send_naws_sb(s);
        }
        break;

    default:
        /* COMMAND, OPTION_DISABLED, PROTO_ERROR not consumed. */
        break;
    }
}

/* Telos write hook: emit IAC sequences and option-negotiation replies
   directly to the host socket. */
static void bridge_write_cb(void *ctx, const uint8 *bytes, uint32 len)
{
    bridge_call_t *s = (bridge_call_t *)ctx;
    if (s->fd >= 0 && len > 0) {
        (void)!write(s->fd, bytes, len);
    }
}

static void send_terminal_type(bridge_call_t *s)
{
    /* RFC 1091 SB TT IS <name>. We use the effective identity
       returned by effective_term_id() so a host that asks via the
       inline DA query and via TTYPE subneg gets the same answer.
       The pre-Telos rotation (PADAWAN -> UNKNOWN) is gone since
       v1.5.0; on repeated SEND requests the host sees the same name
       which it treats per RFC 1091 as the list-exhausted signal. */
    const term_id_entry_t *id;
    const char *name;
    size_t      nlen;
    uint8       body[64];

    if (s->fd < 0) return;
    id   = effective_term_id(s);
    name = id->name;
    nlen = strlen(name);
    if (nlen > sizeof(body) - 1) nlen = sizeof(body) - 1;
    body[0] = TELNET_TTYPE_IS;
    memcpy(body + 1, name, nlen);
    s->ttype_index++;   /* telemetry only */
    telos_send_subneg(&s->telos, TELOS_OPT_TTYPE, body, (uint32)(1 + nlen));
}

static void send_naws_sb(bridge_call_t *s)
{
    /* RFC 1073 SB NAWS body: width_hi width_lo height_hi height_lo.
       Telos handles the IAC IAC body escaping for any 0xFF in the
       dimensions (e.g. width = 255). */
    uint8 body[4];
    if (s->fd < 0) return;
    body[0] = (uint8)(s->naws_width  >> 8);
    body[1] = (uint8)(s->naws_width  & 0xFF);
    body[2] = (uint8)(s->naws_height >> 8);
    body[3] = (uint8)(s->naws_height & 0xFF);
    telos_send_subneg(&s->telos, TELOS_OPT_NAWS, body, 4);
}

static void send_initial_negotiation(bridge_call_t *s)
{
    /* As a telnet client to the host we want:
         DO   SGA / WILL SGA       - char-at-a-time, no GA in either dir
         DO   BINARY / WILL BINARY - 8-bit clean
       Telos's offer_* helpers are idempotent (no-op on YES/WANTYES)
       and gate the WILL/DO emission through Q-state per RFC 1143. */
    telos_offer_do  (&s->telos, TELOS_OPT_SGA);
    telos_offer_will(&s->telos, TELOS_OPT_SGA);
    telos_offer_do  (&s->telos, TELOS_OPT_BINARY);
    telos_offer_will(&s->telos, TELOS_OPT_BINARY);
}

/* --- bridge-specific API ---------------------------------------------- */

void x25_bridge_bind(pad_session_t *p)
{
    g_default_session = p;
}

int x25_bridge_get_fd(void)
{
    int i = find_first_active();
    return (i < 0) ? -1 : g_calls[i].fd;
}

void x25_bridge_set_window_size(uint16 width, uint16 height)
{
    int i;
    g_default_naws_width  = width;
    g_default_naws_height = height;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (g_calls[i].in_use) {
            g_calls[i].naws_width  = width;
            g_calls[i].naws_height = height;
            if (g_calls[i].naws_active &&
                g_calls[i].fd >= 0 && !g_calls[i].connecting) {
                send_naws_sb(&g_calls[i]);
            }
        }
    }
}

/* --- include/x25.h implementation ------------------------------------- */

int x25_init(void)
{
    int i;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        g_calls[i].in_use = 0;
        g_calls[i].fd     = -1;
    }
    return X25_OK;
}

int x25_call(x25_call_t *call, const char *address)
{
    char            host[BRIDGE_HOST_MAX];
    unsigned short  port_v;
    char            port_str[8];
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    int             slot_idx;
    bridge_call_t  *s;
    int             fd;
    int             flags;
    int             rc;

    if (!map_lookup(address, host, sizeof(host), &port_v)) {
        int p = parse_port(address);
        if (p < 0) return X25_ERR_NO_ROUTE;
        port_v = (unsigned short)p;
        strcpy(host, "127.0.0.1");
    }

    {
        uint32 gen;
        slot_idx = alloc_slot(&gen);
        if (slot_idx < 0) return X25_ERR_BUSY;
        s = &g_calls[slot_idx];
        s->session = g_default_session;
        /* Stash for use after we know the call succeeded. */
        /* (we encode into call->call_id at the success points below) */
        (void)gen;
    }

    sprintf(port_str, "%u", (unsigned)port_v);
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port_str, &hints, &result) != 0 || result == NULL) {
        s->in_use = 0;
        return X25_ERR_NO_ROUTE;
    }

    fd = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(result);
        s->in_use = 0;
        return X25_ERR_NO_ROUTE;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    rc = connect(fd, result->ai_addr, result->ai_addrlen);
    freeaddrinfo(result);

    if (rc == 0) {
        s->fd = fd;
        call->call_id   = MAKE_CALL_ID(slot_idx, s->generation);
        call->connected = 1;
        send_initial_negotiation(s);
        return X25_OK;
    }
    if (errno == EINPROGRESS) {
        s->fd         = fd;
        s->connecting = 1;
        call->call_id   = MAKE_CALL_ID(slot_idx, s->generation);
        call->connected = 0;
        return X25_IN_PROGRESS;
    }
    {
        int saved = errno;
        close(fd);
        s->in_use = 0;
        if (saved == ECONNREFUSED) return X25_ERR_REJECTED;
    }
    return X25_ERR_NO_ROUTE;
}

int x25_clear(x25_call_t *call, uint8 cause, uint8 diagnostic)
{
    bridge_call_t *s;
    (void)cause; (void)diagnostic;
    s = call_slot(call);
    if (s != NULL) slot_close(s);
    call->connected = 0;
    return X25_OK;
}

int x25_reset(x25_call_t *call, uint8 cause, uint8 diagnostic)
{
    /* TCP has no analog of X.25 RESET; v1.2 logs and continues. */
    (void)call; (void)cause; (void)diagnostic;
    return X25_OK;
}

int x25_interrupt(x25_call_t *call, uint8 user_data)
{
    static const uint8 iac_ip[] = { 0xFF, 0xF4 };
    bridge_call_t *s = call_slot(call);
    (void)user_data;
    if (s != NULL && s->fd >= 0 && !s->connecting) {
        (void)!write(s->fd, iac_ip, sizeof(iac_ip));
    }
    return X25_OK;
}

int x25_send(x25_call_t *call, const uint8 *data, uint32 len, uint8 qbit)
{
    bridge_call_t *s = call_slot(call);
    uint8  buf[2048];
    uint32 i;
    uint32 j = 0;
    ssize_t n;

    if (s == NULL || s->fd < 0 || s->connecting) return X25_ERR_CLEARED;
    if (len == 0) return X25_OK;
    /* Telnet has no qualified-data analogue, so X.29 PAD messages
       would normally be dropped here. If PCP is enabled and a control
       connection is bound to this call's session, route the X.29
       bytes through PCP as a text event instead. */
    if (qbit) {
        if (pcp_emit_x29_event(s->session, data, len) == 0) {
            return X25_OK;
        }
        return X25_ERR_NOT_SUPPORTED;
    }

    /* RFC 854: literal 0xFF in user data sent as IAC IAC. */
    for (i = 0; i < len; i++) {
        if (j + 2 > sizeof(buf)) {
            n = write(s->fd, buf, j);
            if (n < 0 && errno != EAGAIN && errno != EINTR) {
                return X25_ERR_CLEARED;
            }
            j = 0;
        }
        buf[j++] = data[i];
        if (data[i] == TELNET_IAC) buf[j++] = TELNET_IAC;
    }
    if (j > 0) {
        n = write(s->fd, buf, j);
        if (n < 0 && errno != EAGAIN && errno != EINTR) {
            return X25_ERR_CLEARED;
        }
    }
    return X25_OK;
}

int x25_recv(x25_call_t *call, uint8 *buf, uint32 buf_size,
             uint32 *out_len, uint8 *qbit_out)
{
    (void)call; (void)buf; (void)buf_size;
    *out_len = 0;
    if (qbit_out != NULL) *qbit_out = 0;
    return X25_OK;
}

/* --- driver callback -------------------------------------------------- */

static void poll_one(bridge_call_t *s, short revents)
{
    if (s->fd < 0 || s->session == NULL) return;

    if (s->connecting) {
        int err = 0;
        socklen_t optlen = sizeof(err);
        if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &err, &optlen) != 0) {
            err = errno;
        }
        s->connecting = 0;
        if (err == 0) {
            send_initial_negotiation(s);
            pad_call_connected(s->session);
        } else {
            /* Map common TCP connect errors to X.25 cause codes per
               Recommendation X.25 §5.6.6: 1 = number busy,
               9 = out of order, 13 = not obtainable, 5 = network
               congestion. The diagnostic carries the raw errno so a
               diagnostic byte traces back to the actual POSIX error. */
            pad_clear_cause_t cause = PAD_CLR_NETWORK_PROBLEM;
            uint8             code  = 5;     /* default: network congestion */
            pad_session_t    *sess  = s->session;
            uint8             diag  = (uint8)(err & 0xFF);
            switch (err) {
            case ECONNREFUSED:
                cause = PAD_CLR_NUMBER_NOT_ASSIGNED; code = 13; break;
            case EHOSTUNREACH:
            case ENETUNREACH:
                cause = PAD_CLR_NUMBER_OUT_OF_ORDER; code = 9;  break;
            case ETIMEDOUT:
                cause = PAD_CLR_NETWORK_PROBLEM;     code = 5;  break;
            case EHOSTDOWN:
                cause = PAD_CLR_NUMBER_OUT_OF_ORDER; code = 9;  break;
            default:                                              break;
            }
            slot_close(s);
            pad_remote_cleared(sess, cause, code, diag);
        }
        return;
    }

    if (revents & POLLIN) {
        uint8 raw[1024];
        ssize_t n = read(s->fd, raw, sizeof(raw));
        if (n > 0) {
            /* Hand raw bytes to Telos. It will fire bridge_event_cb
               for each DATA chunk (where we run the ANSI DA query
               interceptor and forward the survivors to the PAD),
               bridge_event_cb for SUBNEG events (TTYPE SEND triggers
               our IS reply), and bridge_event_cb for OPTION_ENABLED
               on local NAWS (we push our dimensions back). */
            telos_recv(&s->telos, raw, (uint32)n);
        } else if (n == 0) {
            /* Clean FIN from remote: code 0 = DTE originated. */
            pad_session_t *sess = s->session;
            slot_close(s);
            pad_remote_cleared(sess, PAD_CLR_REMOTE_REQUEST, 0, 0);
        } else if (errno != EAGAIN && errno != EINTR) {
            /* Mid-call socket error -- ECONNRESET is a remote abort, treat
               like a network problem with diagnostic = errno. */
            pad_session_t *sess = s->session;
            uint8 diag = (uint8)(errno & 0xFF);
            slot_close(s);
            pad_remote_cleared(sess, PAD_CLR_NETWORK_PROBLEM, 5, diag);
        }
        return;
    }

    if (revents & (POLLHUP | POLLERR)) {
        pad_session_t *sess = s->session;
        slot_close(s);
        pad_remote_cleared(sess, PAD_CLR_REMOTE_REQUEST, 0, 0);
    }
}

void x25_bridge_poll_events(short revents)
{
    int i = find_first_active();
    if (i < 0) return;
    poll_one(&g_calls[i], revents);
}

int x25_bridge_fd_for_session(const pad_session_t *p)
{
    int i;
    if (p == NULL) return -1;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (g_calls[i].in_use && g_calls[i].session == p &&
            g_calls[i].fd >= 0) {
            return g_calls[i].fd;
        }
    }
    return -1;
}

void x25_bridge_set_window_size_for_session(const pad_session_t *p,
                                            uint16 width, uint16 height)
{
    int i;
    if (p == NULL) return;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (g_calls[i].in_use && g_calls[i].session == p) {
            g_calls[i].naws_width  = width;
            g_calls[i].naws_height = height;
            if (g_calls[i].naws_active &&
                g_calls[i].fd >= 0 && !g_calls[i].connecting) {
                send_naws_sb(&g_calls[i]);
            }
            return;
        }
    }
}

void x25_bridge_poll_fd(int fd, short revents)
{
    int i;
    if (fd < 0) return;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        if (g_calls[i].in_use && g_calls[i].fd == fd) {
            poll_one(&g_calls[i], revents);
            return;
        }
    }
}

pad_session_t *x25_bridge_session_at_local(const char *ip_str, int port)
{
    int i;
    if (ip_str == NULL) return NULL;
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        struct sockaddr_in local;
        socklen_t          slen = sizeof(local);
        char               buf[INET_ADDRSTRLEN];
        bridge_call_t     *s = &g_calls[i];
        if (!s->in_use || s->fd < 0) continue;
        if (getsockname(s->fd, (struct sockaddr *)&local, &slen) != 0) {
            continue;
        }
        if (local.sin_family != AF_INET) continue;
        if (ntohs(local.sin_port) != (unsigned)port) continue;
        if (inet_ntop(AF_INET, &local.sin_addr, buf, sizeof(buf)) == NULL) {
            continue;
        }
        if (strcmp(buf, ip_str) == 0) return s->session;
    }
    return NULL;
}

int x25_bridge_peer_ip_for_session(const pad_session_t *p,
                                   char *ip_out, uint32 ip_out_sz)
{
    int i;
    if (p == NULL || ip_out == NULL || ip_out_sz < INET_ADDRSTRLEN) {
        return -1;
    }
    for (i = 0; i < BRIDGE_MAX_CALLS; i++) {
        struct sockaddr_in peer;
        socklen_t          slen = sizeof(peer);
        bridge_call_t     *s = &g_calls[i];
        if (!s->in_use || s->fd < 0 || s->session != p) continue;
        if (getpeername(s->fd, (struct sockaddr *)&peer, &slen) != 0) {
            return -1;
        }
        if (peer.sin_family != AF_INET) return -1;
        if (inet_ntop(AF_INET, &peer.sin_addr, ip_out,
                      (socklen_t)ip_out_sz) == NULL) {
            return -1;
        }
        return 0;
    }
    return -1;
}
