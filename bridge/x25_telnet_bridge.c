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

typedef enum {
    IAC_NORMAL = 0,
    IAC_AFTER_IAC,
    IAC_AFTER_VERB,
    IAC_IN_SB,
    IAC_IN_SB_AFTER_IAC
} iac_state_t;

/* RFC 1143 Q-method per-option agreement states. Same shape as in
   bridge/user_telnet.h; declared independently here so the bridge
   directory stays a self-contained extraction unit. */
typedef enum {
    BQ_NO       = 0,
    BQ_YES      = 1,
    BQ_WANTYES  = 2,
    BQ_WANTNO   = 3
} bridge_q_t;

/* Q-state for options 0..31 (covers BINARY=0, ECHO=1, SGA=3,
   TTYPE=24, NAWS=31). Requests for options >= 32 are refused
   statelessly. */
#define BRIDGE_OPT_TABLE 32

typedef struct {
    int            in_use;
    int            fd;
    int            connecting;
    pad_session_t *session;

    /* Generation counter, incremented on alloc. Encoded into the upper
       24 bits of x25_call_t.call_id so a stale handle pointing at a
       reused slot can be detected (call_slot returns NULL on mismatch). */
    uint32         generation;

    /* IAC state machine (per-connection). */
    iac_state_t    iac_state;
    uint8          iac_verb;
    uint8          sb_option;
    uint8          sb_subcmd;
    uint8          sb_seen;

    /* NAWS state (per-connection). */
    int            naws_active;
    uint16         naws_width;
    uint16         naws_height;

    /* RFC 1091 TERMINAL-TYPE rotation index: how many SB TTYPE IS
       responses we've sent on this connection. */
    uint8          ttype_index;

    /* RFC 1143 Q-method per-option agreement state. Required to
       break re-offer/re-reply loops with stateless peers. */
    bridge_q_t     us[BRIDGE_OPT_TABLE];
    bridge_q_t     him[BRIDGE_OPT_TABLE];
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
            g_calls[i].iac_state   = IAC_NORMAL;
            g_calls[i].naws_width  = g_default_naws_width;
            g_calls[i].naws_height = g_default_naws_height;
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
    s->iac_state  = IAC_NORMAL;
    s->iac_verb   = 0;
    s->sb_option  = 0;
    s->sb_subcmd  = 0;
    s->sb_seen    = 0;
    s->naws_active = 0;
    s->ttype_index = 0;
    s->in_use     = 0;
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

/* Forward-declared so send_negotiation_response can push an NAWS SB
   immediately after agreeing WILL NAWS. */
static void send_naws_sb(bridge_call_t *s);

/* Write a 3-byte IAC verb-option sequence on the host socket. */
static void write_iac3_bridge(int fd, uint8 verb, uint8 option)
{
    uint8 buf[3];
    if (fd < 0) return;
    buf[0] = TELNET_IAC;
    buf[1] = verb;
    buf[2] = option;
    (void)!write(fd, buf, 3);
}

/* Policy: which options do WE agree to do for the host? */
static int policy_us_bridge(uint8 option)
{
    return (option == TELNET_OPT_BINARY ||
            option == TELNET_OPT_SGA    ||
            option == TELNET_OPT_TTYPE  ||
            option == TELNET_OPT_NAWS);
}

/* Policy: which options do we want the host to do? */
static int policy_him_bridge(uint8 option)
{
    return (option == TELNET_OPT_BINARY ||
            option == TELNET_OPT_SGA);
}

/* Q-method gated WILL/DO senders. See user_telnet.c for the longer
   commentary on why these matter; same shape, separate state field. */
static void send_will_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) return;
    if (s->us[option] == BQ_YES || s->us[option] == BQ_WANTYES) return;
    s->us[option] = BQ_WANTYES;
    write_iac3_bridge(s->fd, TELNET_WILL, option);
}

static void send_do_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) return;
    if (s->him[option] == BQ_YES || s->him[option] == BQ_WANTYES) return;
    s->him[option] = BQ_WANTYES;
    write_iac3_bridge(s->fd, TELNET_DO, option);
}

/* When we transition us[NAWS] from NO/WANTYES to YES we owe the host
   our current window dimensions as a TELNET SB. Used by both
   send_will_bridge_with_naws (during recv_do) and the initial send. */
static void maybe_push_naws_after_yes(bridge_call_t *s, uint8 option)
{
    if (option == TELNET_OPT_NAWS && s->us[option] == BQ_YES) {
        s->naws_active = 1;
        send_naws_sb(s);
    }
}

static void recv_will_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) {
        write_iac3_bridge(s->fd, TELNET_DONT, option);
        return;
    }
    switch (s->him[option]) {
    case BQ_NO:
        if (policy_him_bridge(option)) {
            s->him[option] = BQ_YES;
            write_iac3_bridge(s->fd, TELNET_DO, option);
        } else {
            write_iac3_bridge(s->fd, TELNET_DONT, option);
        }
        break;
    case BQ_YES:
        break;
    case BQ_WANTYES:
        s->him[option] = BQ_YES;
        break;
    case BQ_WANTNO:
        s->him[option] = BQ_NO;
        break;
    }
}

static void recv_wont_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) return;
    switch (s->him[option]) {
    case BQ_NO:
        break;
    case BQ_YES:
        s->him[option] = BQ_NO;
        write_iac3_bridge(s->fd, TELNET_DONT, option);
        break;
    case BQ_WANTYES:
    case BQ_WANTNO:
        s->him[option] = BQ_NO;
        break;
    }
}

static void recv_do_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) {
        write_iac3_bridge(s->fd, TELNET_WONT, option);
        return;
    }
    switch (s->us[option]) {
    case BQ_NO:
        if (policy_us_bridge(option)) {
            s->us[option] = BQ_YES;
            write_iac3_bridge(s->fd, TELNET_WILL, option);
            maybe_push_naws_after_yes(s, option);
        } else {
            write_iac3_bridge(s->fd, TELNET_WONT, option);
        }
        break;
    case BQ_YES:
        break;
    case BQ_WANTYES:
        s->us[option] = BQ_YES;
        maybe_push_naws_after_yes(s, option);
        break;
    case BQ_WANTNO:
        s->us[option] = BQ_NO;
        break;
    }
}

static void recv_dont_bridge(bridge_call_t *s, uint8 option)
{
    if (option >= BRIDGE_OPT_TABLE) return;
    switch (s->us[option]) {
    case BQ_NO:
        break;
    case BQ_YES:
        s->us[option] = BQ_NO;
        write_iac3_bridge(s->fd, TELNET_WONT, option);
        break;
    case BQ_WANTYES:
    case BQ_WANTNO:
        s->us[option] = BQ_NO;
        break;
    }
}

static void send_negotiation_response(bridge_call_t *s,
                                      uint8 verb, uint8 option)
{
    if      (verb == TELNET_WILL) recv_will_bridge (s, option);
    else if (verb == TELNET_WONT) recv_wont_bridge (s, option);
    else if (verb == TELNET_DO)   recv_do_bridge   (s, option);
    else if (verb == TELNET_DONT) recv_dont_bridge (s, option);
}

static void send_terminal_type(bridge_call_t *s)
{
    static const uint8 prefix[] = {
        TELNET_IAC, TELNET_SB, TELNET_OPT_TTYPE, TELNET_TTYPE_IS
    };
    static const uint8 suffix[] = { TELNET_IAC, TELNET_SE };
    /* RFC 1091: the server may request TTYPE multiple times, expecting
       alternative names. When the client repeats the same name on
       consecutive responses, the server knows the list is exhausted.
       v1.2 rotates through PADAWAN -> UNKNOWN -> UNKNOWN (settled). */
    static const char *const names[] = { "PADAWAN", "UNKNOWN" };
    static const uint8 name_count = (uint8)(sizeof(names) / sizeof(names[0]));
    const char *t;
    size_t      tlen;

    if (s->fd < 0) return;
    if (s->ttype_index >= name_count) {
        t = names[name_count - 1]; /* repeat the last to signal end */
    } else {
        t = names[s->ttype_index];
        s->ttype_index++;
    }
    tlen = strlen(t);
    (void)!write(s->fd, prefix, sizeof(prefix));
    (void)!write(s->fd, t,      tlen);
    (void)!write(s->fd, suffix, sizeof(suffix));
}

static void send_naws_sb(bridge_call_t *s)
{
    uint8 buf[16];
    uint32 j = 0;
    uint8 dims[4];
    int   i;
    if (s->fd < 0) return;
    dims[0] = (uint8)(s->naws_width  >> 8);
    dims[1] = (uint8)(s->naws_width  & 0xFF);
    dims[2] = (uint8)(s->naws_height >> 8);
    dims[3] = (uint8)(s->naws_height & 0xFF);
    buf[j++] = TELNET_IAC;
    buf[j++] = TELNET_SB;
    buf[j++] = TELNET_OPT_NAWS;
    for (i = 0; i < 4; i++) {
        buf[j++] = dims[i];
        if (dims[i] == TELNET_IAC) buf[j++] = TELNET_IAC;
    }
    buf[j++] = TELNET_IAC;
    buf[j++] = TELNET_SE;
    (void)!write(s->fd, buf, j);
}

static void send_initial_negotiation(bridge_call_t *s)
{
    /* As a telnet client to the host we want:
         DO   SGA / WILL SGA       - char-at-a-time, no GA in either dir
         DO   BINARY / WILL BINARY - 8-bit clean
       The pre-Q-method version also blasted DONT ECHO + WONT ECHO
       unconditionally; those are redundant per RFC 1143 when both
       sides start at Q_NO for ECHO (peer can't be echoing yet), so
       we drop them. If the host later OFFERS WILL ECHO we'll refuse
       it via policy_him_bridge, which excludes ECHO. */
    send_do_bridge  (s, TELNET_OPT_SGA);
    send_will_bridge(s, TELNET_OPT_SGA);
    send_do_bridge  (s, TELNET_OPT_BINARY);
    send_will_bridge(s, TELNET_OPT_BINARY);
}

static uint32 filter_iac(bridge_call_t *s,
                         const uint8 *in, uint32 in_len, uint8 *out)
{
    uint32 i, j = 0;
    for (i = 0; i < in_len; i++) {
        uint8 b = in[i];
        switch (s->iac_state) {
        case IAC_NORMAL:
            if (b == TELNET_IAC) {
                s->iac_state = IAC_AFTER_IAC;
            } else {
                out[j++] = b;
            }
            break;
        case IAC_AFTER_IAC:
            if (b == TELNET_IAC) {
                out[j++] = TELNET_IAC;
                s->iac_state = IAC_NORMAL;
            } else if (b == TELNET_WILL || b == TELNET_WONT ||
                       b == TELNET_DO   || b == TELNET_DONT) {
                s->iac_verb  = b;
                s->iac_state = IAC_AFTER_VERB;
            } else if (b == TELNET_SB) {
                s->sb_option = 0;
                s->sb_subcmd = 0;
                s->sb_seen   = 0;
                s->iac_state = IAC_IN_SB;
            } else {
                s->iac_state = IAC_NORMAL;
            }
            break;
        case IAC_AFTER_VERB:
            send_negotiation_response(s, s->iac_verb, b);
            s->iac_state = IAC_NORMAL;
            break;
        case IAC_IN_SB:
            if (b == TELNET_IAC) {
                s->iac_state = IAC_IN_SB_AFTER_IAC;
            } else {
                if (s->sb_seen == 0)      s->sb_option = b;
                else if (s->sb_seen == 1) s->sb_subcmd = b;
                if (s->sb_seen < 255)     s->sb_seen++;
            }
            break;
        case IAC_IN_SB_AFTER_IAC:
            if (b == TELNET_SE) {
                if (s->sb_option == TELNET_OPT_TTYPE &&
                    s->sb_subcmd == TELNET_TTYPE_SEND) {
                    send_terminal_type(s);
                }
                s->iac_state = IAC_NORMAL;
            } else {
                s->iac_state = IAC_IN_SB;
            }
            break;
        }
    }
    return j;
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
        uint8 filtered[1024];
        uint32 n_filtered;
        ssize_t n = read(s->fd, raw, sizeof(raw));
        if (n > 0) {
            n_filtered = filter_iac(s, raw, (uint32)n, filtered);
            if (n_filtered > 0) {
                /* User-data flow from the Telnet peer is always qbit=0;
                   true X.29 needs a real X.25 transport. */
                pad_input_remote(s->session, filtered, n_filtered, 0);
            }
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
