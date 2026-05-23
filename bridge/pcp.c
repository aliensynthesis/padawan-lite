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

#define _POSIX_C_SOURCE 200809L

#include "pcp.h"
#include "x25.h"
#include "x25_telnet_bridge.h"
#include "x28_signals.h"
#include "x29.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define PCP_RECV_BUF  512
#define PCP_LINE_MAX  256

typedef struct {
    int            in_use;
    int            fd;
    pad_session_t *bound;
    char           host_ip[INET_ADDRSTRLEN];  /* control-conn source */
    uint8          rx_buf[PCP_RECV_BUF];
    uint32         rx_len;
} pcp_conn_t;

static pcp_conn_t g_conns[PCP_MAX_CONNS];
static int        g_listen_fd = -1;

/* ------------------------------------------------------------------------- */
/* setup / teardown                                                          */
/* ------------------------------------------------------------------------- */

int pcp_init(int port)
{
    struct sockaddr_in addr;
    int  fd;
    int  one = 1;
    int  flags;
    int  i;

    for (i = 0; i < PCP_MAX_CONNS; i++) {
        memset(&g_conns[i], 0, sizeof(g_conns[i]));
        g_conns[i].fd = -1;
    }
    g_listen_fd = -1;
    if (port <= 0) return 0;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    /* Localhost-only by default for safety. */
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) {
        close(fd);
        return -1;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    g_listen_fd = fd;
    return 0;
}

void pcp_shutdown(void)
{
    int i;
    for (i = 0; i < PCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use && g_conns[i].fd >= 0) {
            close(g_conns[i].fd);
        }
        memset(&g_conns[i], 0, sizeof(g_conns[i]));
        g_conns[i].fd = -1;
    }
    if (g_listen_fd >= 0) close(g_listen_fd);
    g_listen_fd = -1;
}

int pcp_enabled(void)        { return g_listen_fd >= 0; }
int pcp_listener_fd(void)    { return g_listen_fd; }

int pcp_collect_pollfds(struct pollfd *out, int cap)
{
    int i;
    int n = 0;
    if (out == NULL || cap <= 0) return 0;
    for (i = 0; i < PCP_MAX_CONNS && n < cap; i++) {
        if (!g_conns[i].in_use || g_conns[i].fd < 0) continue;
        out[n].fd      = g_conns[i].fd;
        out[n].events  = POLLIN;
        out[n].revents = 0;
        n++;
    }
    return n;
}

void pcp_unbind_session(const pad_session_t *p)
{
    int i;
    for (i = 0; i < PCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use && g_conns[i].bound == p) {
            g_conns[i].bound = NULL;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* low-level i/o helpers                                                     */
/* ------------------------------------------------------------------------- */

static void conn_write(pcp_conn_t *c, const char *s)
{
    uint32 n;
    if (c == NULL || c->fd < 0 || s == NULL) return;
    n = (uint32)strlen(s);
    (void)!write(c->fd, s, (size_t)n);
}

static void conn_writeln(pcp_conn_t *c, const char *s)
{
    conn_write(c, s);
    conn_write(c, "\r\n");
}

static void conn_close(pcp_conn_t *c)
{
    if (c == NULL) return;
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof(*c));
    c->fd = -1;
}

static pcp_conn_t *conn_alloc(void)
{
    int i;
    for (i = 0; i < PCP_MAX_CONNS; i++) {
        if (!g_conns[i].in_use) return &g_conns[i];
    }
    return NULL;
}

static pcp_conn_t *conn_by_fd(int fd)
{
    int i;
    for (i = 0; i < PCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use && g_conns[i].fd == fd) return &g_conns[i];
    }
    return NULL;
}

static pcp_conn_t *conn_by_session(const pad_session_t *p)
{
    int i;
    if (p == NULL) return NULL;
    for (i = 0; i < PCP_MAX_CONNS; i++) {
        if (g_conns[i].in_use && g_conns[i].bound == p) return &g_conns[i];
    }
    return NULL;
}

/* ------------------------------------------------------------------------- */
/* text-protocol parser helpers                                              */
/* ------------------------------------------------------------------------- */

static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

/* Parse comma-separated decimal pairs "ref:val,ref:val,...". Returns
   number of pairs parsed, or -1 on syntax error. Stops at end-of-string. */
static int parse_pairs(const char *s, x29_pair_t *pairs, int max)
{
    int n = 0;
    s = skip_ws(s);
    while (*s != '\0' && n < max) {
        long ref;
        long val;
        char *end;
        ref = strtol(s, &end, 10);
        if (end == s || ref < 0 || ref > 255) return -1;
        s = skip_ws(end);
        if (*s != ':') return -1;
        s++;
        s = skip_ws(s);
        val = strtol(s, &end, 10);
        if (end == s || val < 0 || val > 255) return -1;
        pairs[n].ref   = (uint8)ref;
        pairs[n].value = (uint8)val;
        n++;
        s = skip_ws(end);
        if (*s == ',') { s++; s = skip_ws(s); continue; }
        if (*s == '\0') break;
        return -1;
    }
    return n;
}

/* Parse comma-separated decimal refs (for READ). */
static int parse_refs(const char *s, uint8 *refs, int max)
{
    int n = 0;
    s = skip_ws(s);
    while (*s != '\0' && n < max) {
        long ref;
        char *end;
        ref = strtol(s, &end, 10);
        if (end == s || ref < 1 || ref > 255) return -1;
        refs[n++] = (uint8)ref;
        s = skip_ws(end);
        if (*s == ',') { s++; s = skip_ws(s); continue; }
        if (*s == '\0') break;
        return -1;
    }
    return n;
}

/* Format X.29 pairs into a comma-separated "ref:val,..." string into
   out. Returns bytes written (excluding NUL), or -1 on overflow. */
static int32 format_pairs(char *out, uint32 cap,
                          const x29_pair_t *pairs, uint8 count)
{
    uint32 pos = 0;
    uint8  i;
    int    n;
    for (i = 0; i < count; i++) {
        n = snprintf(out + pos, cap - pos, "%s%u:%u",
                     i == 0 ? "" : ",",
                     (unsigned)pairs[i].ref, (unsigned)pairs[i].value);
        if (n < 0 || (uint32)n >= cap - pos) return -1;
        pos += (uint32)n;
    }
    out[pos] = '\0';
    return (int32)pos;
}

static int32 format_refs(char *out, uint32 cap,
                         const x29_pair_t *pairs, uint8 count)
{
    uint32 pos = 0;
    uint8  i;
    int    n;
    for (i = 0; i < count; i++) {
        n = snprintf(out + pos, cap - pos, "%s%u",
                     i == 0 ? "" : ",", (unsigned)pairs[i].ref);
        if (n < 0 || (uint32)n >= cap - pos) return -1;
        pos += (uint32)n;
    }
    out[pos] = '\0';
    return (int32)pos;
}

/* ------------------------------------------------------------------------- */
/* command dispatch                                                          */
/* ------------------------------------------------------------------------- */

/* Build an X.29 message body and feed it to the PAD as if it had
   arrived on a qbit=1 packet. Used by SET, READ, SETREAD, PAR, ICLR,
   BREAK, ERR commands. */
static void feed_x29(pad_session_t *p, const uint8 *body, uint32 len)
{
    if (p == NULL) return;
    pad_input_remote(p, body, len, 1);
}

/* Case-insensitive compare-against-keyword that requires the keyword to
   be followed by whitespace or end-of-string. Returns the position
   after the keyword on success, or NULL on no match. */
static const char *match_kw(const char *s, const char *kw)
{
    uint32 i;
    for (i = 0; kw[i] != '\0'; i++) {
        char a = s[i];
        if (a >= 'a' && a <= 'z') a = (char)(a - 'a' + 'A');
        if (a != kw[i]) return NULL;
    }
    if (s[i] == '\0' || s[i] == ' ' || s[i] == '\t') return s + i;
    return NULL;
}

static int handle_bind(pcp_conn_t *c, const char *arg)
{
    char  ip[INET_ADDRSTRLEN];
    char  data_peer[INET_ADDRSTRLEN];
    int   port;
    const char *p;
    const char *colon;
    uint32 ip_len;
    pad_session_t *sess;

    arg = skip_ws(arg);
    colon = strchr(arg, ':');
    if (colon == NULL) {
        conn_writeln(c, "ERR usage: BIND <ip>:<port>");
        return 0;
    }
    ip_len = (uint32)(colon - arg);
    if (ip_len == 0 || ip_len >= sizeof(ip)) {
        conn_writeln(c, "ERR bad ip");
        return 0;
    }
    memcpy(ip, arg, ip_len);
    ip[ip_len] = '\0';
    p = colon + 1;
    port = atoi(p);
    if (port <= 0 || port > 65535) {
        conn_writeln(c, "ERR bad port");
        return 0;
    }

    sess = x25_bridge_session_at_local(ip, port);
    if (sess == NULL) {
        conn_writeln(c, "ERR no-such-session");
        return 0;
    }
    /* Security: require the PCP source IP to match the data-conn peer. */
    if (x25_bridge_peer_ip_for_session(sess, data_peer,
                                       (uint32)sizeof(data_peer)) != 0) {
        conn_writeln(c, "ERR session-has-no-peer");
        return 0;
    }
    if (strcmp(data_peer, c->host_ip) != 0) {
        conn_writeln(c, "ERR source-ip-mismatch");
        return 0;
    }
    if (conn_by_session(sess) != NULL) {
        conn_writeln(c, "ERR session-already-bound");
        return 0;
    }
    c->bound = sess;
    conn_writeln(c, "OK bound");
    return 0;
}

static int handle_set(pcp_conn_t *c, const char *arg)
{
    x29_pair_t pairs[X29_MAX_PAIRS];
    uint8      buf[1 + X29_MAX_PAIRS * 2];
    int        n;
    int32      m;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    n = parse_pairs(arg, pairs, X29_MAX_PAIRS);
    if (n <= 0)            { conn_writeln(c, "ERR syntax");    return 0; }
    m = x29_encode_set(pairs, (uint8)n, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_setread(pcp_conn_t *c, const char *arg)
{
    x29_pair_t pairs[X29_MAX_PAIRS];
    uint8      buf[1 + X29_MAX_PAIRS * 2];
    int        n;
    int32      m;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    n = parse_pairs(arg, pairs, X29_MAX_PAIRS);
    if (n <= 0)            { conn_writeln(c, "ERR syntax");    return 0; }
    m = x29_encode_set_read(pairs, (uint8)n, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_par(pcp_conn_t *c, const char *arg)
{
    x29_pair_t pairs[X29_MAX_PAIRS];
    uint8      buf[1 + X29_MAX_PAIRS * 2];
    int        n;
    int32      m;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    n = parse_pairs(arg, pairs, X29_MAX_PAIRS);
    if (n <= 0)            { conn_writeln(c, "ERR syntax");    return 0; }
    m = x29_encode_parameter_ind(pairs, (uint8)n, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_read(pcp_conn_t *c, const char *arg)
{
    uint8 refs[X29_MAX_PAIRS];
    uint8 buf[1 + X29_MAX_PAIRS];
    int   n;
    int32 m;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    n = parse_refs(arg, refs, X29_MAX_PAIRS);
    if (n <= 0)            { conn_writeln(c, "ERR syntax");    return 0; }
    m = x29_encode_read(refs, (uint8)n, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_iclr(pcp_conn_t *c, const char *arg)
{
    uint8 buf[2];
    int32 m;
    (void)arg;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    m = x29_encode_invite_clear(buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_break(pcp_conn_t *c, const char *arg)
{
    uint8 buf[3];
    int32 m;
    int   has = 0;
    long  val = 0;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    arg = skip_ws(arg);
    if (*arg != '\0') {
        char *end;
        val = strtol(arg, &end, 10);
        if (end == arg || val < 0 || val > 255) {
            conn_writeln(c, "ERR syntax");
            return 0;
        }
        has = 1;
    }
    m = x29_encode_break((uint8)has, (uint8)val, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static int handle_err(pcp_conn_t *c, const char *arg)
{
    uint8 buf[3];
    int32 m;
    long  code;
    char *end;
    if (c->bound == NULL) { conn_writeln(c, "ERR not-bound"); return 0; }
    arg = skip_ws(arg);
    code = strtol(arg, &end, 10);
    if (end == arg || code < 0 || code > 255) {
        conn_writeln(c, "ERR syntax");
        return 0;
    }
    m = x29_encode_error((uint8)code, 0, buf, sizeof(buf));
    if (m <= 0)            { conn_writeln(c, "ERR encode");    return 0; }
    feed_x29(c->bound, buf, (uint32)m);
    conn_writeln(c, "OK");
    return 0;
}

static void dispatch_line(pcp_conn_t *c, const char *line)
{
    const char *rest;
    rest = match_kw(line, "BIND");      if (rest) { handle_bind(c, rest);     return; }
    rest = match_kw(line, "SET");       if (rest) { handle_set(c, rest);      return; }
    rest = match_kw(line, "SETREAD");   if (rest) { handle_setread(c, rest);  return; }
    rest = match_kw(line, "PAR");       if (rest) { handle_par(c, rest);      return; }
    rest = match_kw(line, "READ");      if (rest) { handle_read(c, rest);     return; }
    rest = match_kw(line, "ICLR");      if (rest) { handle_iclr(c, rest);     return; }
    rest = match_kw(line, "BREAK");     if (rest) { handle_break(c, rest);    return; }
    rest = match_kw(line, "ERR");       if (rest) { handle_err(c, rest);      return; }
    rest = match_kw(line, "QUIT");      if (rest) {
        conn_writeln(c, "OK bye");
        conn_close(c);
        return;
    }
    conn_writeln(c, "ERR unknown-command");
}

/* ------------------------------------------------------------------------- */
/* connection-side input                                                     */
/* ------------------------------------------------------------------------- */

void pcp_handle_accept(void)
{
    struct sockaddr_in peer;
    socklen_t          slen = sizeof(peer);
    int                fd;
    int                flags;
    pcp_conn_t        *c;

    if (g_listen_fd < 0) return;
    fd = accept(g_listen_fd, (struct sockaddr *)&peer, &slen);
    if (fd < 0) return;

    c = conn_alloc();
    if (c == NULL) {
        static const char busy[] = "ERR pcp-busy\r\n";
        (void)!write(fd, busy, sizeof(busy) - 1);
        close(fd);
        return;
    }
    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    c->in_use = 1;
    c->fd     = fd;
    c->bound  = NULL;
    c->rx_len = 0;
    if (peer.sin_family == AF_INET) {
        (void)inet_ntop(AF_INET, &peer.sin_addr, c->host_ip,
                        (socklen_t)sizeof(c->host_ip));
    } else {
        c->host_ip[0] = '\0';
    }
    conn_writeln(c, "OK pcp ready");
}

static void process_line(pcp_conn_t *c, char *line)
{
    /* Trim trailing CR (handles CRLF input) and leading whitespace. */
    uint32 n = (uint32)strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == ' ' ||
                     line[n - 1] == '\t')) {
        line[--n] = '\0';
    }
    while (line[0] == ' ' || line[0] == '\t') line++;
    if (line[0] == '\0' || line[0] == '#') return;   /* empty / comment */
    dispatch_line(c, line);
}

void pcp_handle_conn(int fd, short revents)
{
    pcp_conn_t *c = conn_by_fd(fd);
    ssize_t     n;
    uint32      i;
    if (c == NULL) return;
    if (revents & (POLLHUP | POLLERR)) {
        conn_close(c);
        return;
    }
    if (!(revents & POLLIN)) return;

    n = read(c->fd, c->rx_buf + c->rx_len,
             (size_t)(PCP_RECV_BUF - c->rx_len));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        conn_close(c);
        return;
    }
    if (n == 0) { conn_close(c); return; }
    c->rx_len += (uint32)n;

    /* Pull out every complete LF-terminated line. */
    for (;;) {
        char  *nl = memchr(c->rx_buf, '\n', c->rx_len);
        char   line[PCP_LINE_MAX];
        uint32 line_len;
        if (nl == NULL) break;
        line_len = (uint32)(nl - (char *)c->rx_buf);
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, c->rx_buf, line_len);
        line[line_len] = '\0';
        /* shift the rest down */
        i = (uint32)(nl - (char *)c->rx_buf) + 1;
        memmove(c->rx_buf, c->rx_buf + i, c->rx_len - i);
        c->rx_len -= i;
        process_line(c, line);
        if (!c->in_use) return;   /* QUIT closed us */
    }
    /* Drop pathological no-LF lines that would overflow the buffer. */
    if (c->rx_len >= PCP_RECV_BUF) {
        c->rx_len = 0;
        conn_writeln(c, "ERR line-too-long");
    }
}

/* ------------------------------------------------------------------------- */
/* outbound: X.29 from PAD -> PCP host as EVT lines                          */
/* ------------------------------------------------------------------------- */

int pcp_emit_x29_event(const pad_session_t *p, const uint8 *body, uint32 len)
{
    pcp_conn_t   *c = conn_by_session(p);
    x29_message_t msg;
    char          out[PCP_LINE_MAX];
    char          pairs[PCP_LINE_MAX];
    int           ok = 0;

    if (c == NULL) return -1;
    if (x29_decode(body, len, &msg) != 0) {
        snprintf(out, sizeof(out), "EVT UNKNOWN");
        conn_writeln(c, out);
        return 0;
    }

    switch (msg.type) {
    case X29_MSG_INVITE_CLEAR:
        conn_writeln(c, "EVT ICLR");
        ok = 1;
        break;
    case X29_MSG_SET:
        if (format_pairs(pairs, sizeof(pairs),
                         msg.pairs, msg.pair_count) >= 0) {
            snprintf(out, sizeof(out), "EVT SET %s", pairs);
            conn_writeln(c, out);
            ok = 1;
        }
        break;
    case X29_MSG_SET_READ:
        if (format_pairs(pairs, sizeof(pairs),
                         msg.pairs, msg.pair_count) >= 0) {
            snprintf(out, sizeof(out), "EVT SETREAD %s", pairs);
            conn_writeln(c, out);
            ok = 1;
        }
        break;
    case X29_MSG_PARAMETER_IND:
        if (format_pairs(pairs, sizeof(pairs),
                         msg.pairs, msg.pair_count) >= 0) {
            snprintf(out, sizeof(out), "EVT PAR %s", pairs);
            conn_writeln(c, out);
            ok = 1;
        }
        break;
    case X29_MSG_READ:
        if (format_refs(pairs, sizeof(pairs),
                        msg.pairs, msg.pair_count) >= 0) {
            snprintf(out, sizeof(out), "EVT READ %s", pairs);
            conn_writeln(c, out);
            ok = 1;
        }
        break;
    case X29_MSG_BREAK:
        if (msg.break_has_param8) {
            snprintf(out, sizeof(out), "EVT BREAK %u",
                     (unsigned)msg.break_param8);
        } else {
            snprintf(out, sizeof(out), "EVT BREAK");
        }
        conn_writeln(c, out);
        ok = 1;
        break;
    case X29_MSG_ERROR:
        snprintf(out, sizeof(out), "EVT ERR %u",
                 (unsigned)msg.error_code);
        conn_writeln(c, out);
        ok = 1;
        break;
    case X29_MSG_RESELECTION:
    case X29_MSG_UNKNOWN:
    default:
        conn_writeln(c, "EVT UNKNOWN");
        ok = 1;
        break;
    }
    (void)ok;
    return 0;
}
