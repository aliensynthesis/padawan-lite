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

/* Bridge driver: routes user PADs to TCP-backed X.25 calls.

   Two modes:
     - default (no --listen): single session bound to stdin/stdout.
       Termios raw mode applied when stdin is a tty. This is the
       interactive PAD demo, identical to v1.1's original driver.
     - --listen PORT: TCP server. Accepts incoming user connections,
       each becomes its own pad_session_t with its own x25_call_t.
       v1.1 caps concurrent sessions at MAX_SESSIONS = 16.

   User-side connections in --listen mode are raw TCP (no Telnet IAC
   handling). Users connect with e.g. 'nc <host> <port>'. Adding
   Telnet IAC negotiation on the user side is deferred. */

#define _POSIX_C_SOURCE 200809L

#include "pad.h"
#include "x3.h"
#include "x25.h"
#include "x25_telnet_bridge.h"
#include "x28_signals.h"
#include "user_telnet.h"
#include "pcp.h"
#include "platform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <poll.h>
#include <time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <fcntl.h>

#define MAX_SESSIONS 16
#define MAX_NUIS     64
#define NUI_MAX_LEN  31
#define THROTTLE_BUF        4096   /* per-direction per-session ring buffer */
#define THROTTLE_HIGH_WATER 2048   /* suppress upstream reads at/above this */
#define TRACE_BUF           4096   /* per-session trace accumulator */
#define TRACE_PREFIX_MAX     127   /* --trace-prefix max length */
#define TRACE_DEFAULT_PREFIX "padawan-lite-trace"

/* Token-bucket throttle queue (one per direction per session). budget
   is in milli-bytes so we can pace fractional bytes per 50 ms tick at
   low baud rates (300 bps -> 30 cps -> 1.5 chars/tick). */
typedef struct {
    uint8  buf[THROTTLE_BUF];
    uint32 head;
    uint32 len;
    int32  budget;
} throttle_q_t;

typedef struct {
    int            in_use;
    int            is_stdin;
    int            read_fd;     /* read user bytes from here */
    int            write_fd;    /* write PAD output here */
    pad_session_t  pad;         /* the call handle for this session is
                                    pad.call (set by x25_call inside
                                    Padawan-Lite's SELECTION dispatch) */
    /* Telnet IAC state for the user socket. Only used in --listen
       mode (is_stdin == 0). Stdin sessions go straight to the PAD
       without filtering. */
    user_telnet_t  telnet;
    /* Output-side throttling (only used when g_throttle_bps > 0).
       dte_q paces PAD -> user (downstream); remote_q paces PAD -> host
       (upstream). User -> PAD and host -> PAD are NOT throttled --
       both inputs are gated by the output side via the PAD's normal
       buffering, which is enough for a "feels like N baud" experience. */
    throttle_q_t   dte_q;
    throttle_q_t   remote_q;
    /* Inbound traffic logging (only used when g_trace_enabled).
       Default mode: one buffer that accumulates whichever direction
       is current; on direction transition the buffer is flushed.
       Line mode (--trace-line-mode): two buffers, one per direction;
       CLIENT bytes flush on CR (with paired SERVICE flush before),
       so each prompt+echo and the matching typed line emit as a pair. */
    FILE          *trace_fp;
    int            trace_last_dir;
    uint8          trace_buf[TRACE_BUF];           /* default-mode buffer */
    uint32         trace_buf_len;
    uint8          trace_line_dte[TRACE_BUF];      /* line-mode CLIENT */
    uint32         trace_line_dte_len;
    uint8          trace_line_remote[TRACE_BUF];   /* line-mode SERVICE */
    uint32         trace_line_remote_len;
} user_session_t;

static user_session_t        g_sessions[MAX_SESSIONS];
static int                   g_listen_fd = -1;
static struct termios        g_orig_termios;
static int                   g_termios_saved = 0;
static volatile sig_atomic_t g_winch_pending = 0;

/* --- NUI auth (set via --auth <file>) -------------------------------- */

static char g_nuis[MAX_NUIS][NUI_MAX_LEN + 1];
static int  g_nui_count   = 0;
static int  g_auth_active = 0;
/* When --telnet-defaults / -t is set, sessions override profile params
   2/3/4 with 0/0/1 (no PAD echo, no forwarding char, idle = 1) so an
   interactive Telnet user gets char-at-a-time, no double-echo behaviour. */
static int  g_telnet_defaults = 0;

/* Wire-rate throttle. Set by --baud N (bits/sec, 0 = unlimited).
   Bytes/sec assumes the conventional 10 bits per character (1 start +
   8 data + 1 stop) so the user thinks in familiar 300 / 1200 / 2400
   numbers. Throttle is per-session and applies in both directions. */
static int32 g_throttle_bps = 0;       /* bytes per second; 0 = off */
static int32 g_throttle_burst_mb = 0;  /* burst cap, milli-bytes */

/* Inbound-traffic logging. --trace enables with default prefix;
   --trace-prefix <prefix> overrides and also enables. Each session
   gets its own file named "<prefix>-<unix-ts>-<seq>.log". */
static int  g_trace_enabled    = 0;
static int  g_trace_line_mode  = 0;
static int  g_pcp_port         = 0;   /* PCP listener port; 0 = disabled */
static char g_trace_prefix[TRACE_PREFIX_MAX + 1] = TRACE_DEFAULT_PREFIX;
static int  g_trace_seq        = 0;       /* monotonic session counter */

/* Forward declarations for trace helpers (defined further down so the
   PAD-callback section can call them). */
static void trace_open(user_session_t *u, int is_stdin);
static void trace_close(user_session_t *u);
static void trace_callback(void *ctx, int direction,
                           const uint8 *data, uint32 len);

static int load_nui_file(const char *filename)
{
    FILE *f;
    char  line[128];
    f = fopen(filename, "r");
    if (f == NULL) return -1;
    g_nui_count = 0;
    while (g_nui_count < MAX_NUIS && fgets(line, sizeof(line), f) != NULL) {
        char  *p;
        size_t n;
        p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;
        n = strlen(p);
        while (n > 0 && (p[n - 1] == '\n' || p[n - 1] == '\r' ||
                         p[n - 1] == ' '  || p[n - 1] == '\t')) n--;
        if (n == 0 || n > NUI_MAX_LEN) continue;
        memcpy(g_nuis[g_nui_count], p, n);
        g_nuis[g_nui_count][n] = '\0';
        g_nui_count++;
    }
    fclose(f);
    return 0;
}

static int nui_in_list(const char *nui)
{
    int j;
    if (nui == NULL || nui[0] == '\0') return 0;
    for (j = 0; j < g_nui_count; j++) {
        if (strcmp(nui, g_nuis[j]) == 0) return 1;
    }
    return 0;
}

static int nui_check_cb(void *ctx,
                        const struct x28_facility_t *facilities,
                        uint8 facility_count,
                        const char *address,
                        const char *session_nui)
{
    uint8 i;
    (void)ctx;
    (void)address;
    /* Per-call NUI (selection-signal N facility, X.28 §3.5.15.1.1)
       takes precedence over session-level NUI when both are present.
       Reject if neither is set or neither matches the allow-list. */
    for (i = 0; i < facility_count; i++) {
        if (facilities[i].code == 'N') {
            return nui_in_list(facilities[i].arg) ? 0 : -1;
        }
    }
    if (nui_in_list(session_nui)) return 0;
    return -1;
}

static void apply_telnet_defaults(pad_session_t *p)
{
    /* Direct param mutation: a user can SET these at runtime anyway,
       so writing them at session start is equivalent to issuing
       "SET 2:0, 3:0, 4:1" before any data flows. Documented as a
       deviation from the simple profile in deviations.txt. */
    p->params.values[X3_PAR_ECHO]    = 0;
    p->params.values[X3_PAR_FORWARD] = 0;
    p->params.values[X3_PAR_IDLE]    = 1;
}

/* --- termios / signal helpers (only used for stdin mode) ------------- */

static void restore_termios(void)
{
    if (g_termios_saved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &g_orig_termios);
        g_termios_saved = 0;
    }
}

static void on_signal(int sig)
{
    restore_termios();
    _exit(128 + sig);
}

static void on_winch(int sig)
{
    (void)sig;
    g_winch_pending = 1;
}

static void enter_raw_mode(void)
{
    struct termios raw;
    if (!isatty(STDIN_FILENO)) return;
    if (tcgetattr(STDIN_FILENO, &g_orig_termios) != 0) return;
    raw = g_orig_termios;
    raw.c_lflag &= ~((tcflag_t)(ICANON | ECHO));
    raw.c_iflag &= ~((tcflag_t)(ICRNL | IXON));
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) return;
    g_termios_saved = 1;
    atexit(restore_termios);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGHUP,  on_signal);
}

static void push_window_size(void)
{
    struct winsize ws;
    if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 &&
        ws.ws_col > 0 && ws.ws_row > 0) {
        x25_bridge_set_window_size((uint16)ws.ws_col, (uint16)ws.ws_row);
    }
}

static uint32 now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32)ts.tv_sec * 1000U + (uint32)(ts.tv_nsec / 1000000L);
}

/* --- session table ---------------------------------------------------- */

static user_session_t *alloc_session(void)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) {
            memset(&g_sessions[i], 0, sizeof(g_sessions[i]));
            g_sessions[i].in_use   = 1;
            g_sessions[i].read_fd  = -1;
            g_sessions[i].write_fd = -1;
            return &g_sessions[i];
        }
    }
    return NULL;
}

static void destroy_session(user_session_t *u)
{
    if (!u || !u->in_use) return;
    /* Release any PCP control connection bound to this session before
       the pad_session_t goes away. */
    pcp_unbind_session(&u->pad);
    /* Close the bridge call (if any) before freeing the session, so
       the TCP socket on the X.25 side doesn't leak. */
    if (u->pad.call.connected) {
        x25_clear(&u->pad.call, 0, 0);
    }
    trace_close(u);
    if (!u->is_stdin && u->read_fd >= 0) {
        close(u->read_fd);
    }
    memset(u, 0, sizeof(*u));
    u->read_fd  = -1;
    u->write_fd = -1;
}

/* --- throttle helpers ------------------------------------------------ */

static void throttle_enqueue(throttle_q_t *q, const uint8 *data, uint32 len)
{
    while (len > 0 && q->len < THROTTLE_BUF) {
        uint32 idx;
        uint32 chunk;
        uint32 space;
        idx   = (q->head + q->len) % THROTTLE_BUF;
        space = THROTTLE_BUF - q->len;
        chunk = len;
        if (chunk > space)              chunk = space;
        if (idx + chunk > THROTTLE_BUF) chunk = THROTTLE_BUF - idx;
        memcpy(q->buf + idx, data, chunk);
        q->len += chunk;
        data   += chunk;
        len    -= chunk;
    }
    /* If len > 0 here the buffer was full; bytes are dropped. At 300 baud
       with a 4 KB buffer this would take ~17 minutes of unread output to
       hit; documented in deviations.txt. */
}

/* Write n bytes from the head of q to the wire via the supplied writer
   (which performs IAC escaping etc.). Returns bytes actually emitted. */
static uint32 throttle_emit_dte(user_session_t *u, uint32 n)
{
    throttle_q_t *q = &u->dte_q;
    uint32 emitted = 0;
    while (n > 0 && q->len > 0) {
        uint32 chunk = q->len;
        if (chunk > n)                            chunk = n;
        if (q->head + chunk > THROTTLE_BUF)       chunk = THROTTLE_BUF - q->head;
        if (u->is_stdin) {
            (void)!write(u->write_fd, q->buf + q->head, (size_t)chunk);
        } else {
            user_telnet_write(u->write_fd, q->buf + q->head, chunk);
        }
        q->head = (q->head + chunk) % THROTTLE_BUF;
        q->len -= chunk;
        n      -= chunk;
        emitted += chunk;
    }
    return emitted;
}

static uint32 throttle_emit_remote(user_session_t *u, uint32 n)
{
    throttle_q_t *q = &u->remote_q;
    uint32 emitted = 0;
    while (n > 0 && q->len > 0) {
        uint32 chunk = q->len;
        if (chunk > n)                            chunk = n;
        if (q->head + chunk > THROTTLE_BUF)       chunk = THROTTLE_BUF - q->head;
        (void)x25_send(&u->pad.call, q->buf + q->head, chunk, 0);
        q->head = (q->head + chunk) % THROTTLE_BUF;
        q->len -= chunk;
        n      -= chunk;
        emitted += chunk;
    }
    return emitted;
}

static void throttle_refill(throttle_q_t *q, int32 elapsed_ms)
{
    q->budget += g_throttle_bps * elapsed_ms;
    if (q->budget > g_throttle_burst_mb) q->budget = g_throttle_burst_mb;
}

/* Drain whatever the current budget allows. Returns bytes drained. */
static uint32 throttle_drain_dte(user_session_t *u)
{
    int32  allowed_bytes = u->dte_q.budget / 1000;
    uint32 sent;
    if (allowed_bytes <= 0 || u->dte_q.len == 0) return 0;
    sent = throttle_emit_dte(u, (uint32)allowed_bytes);
    u->dte_q.budget -= (int32)sent * 1000;
    return sent;
}

static uint32 throttle_drain_remote(user_session_t *u)
{
    int32  allowed_bytes = u->remote_q.budget / 1000;
    uint32 sent;
    if (allowed_bytes <= 0 || u->remote_q.len == 0) return 0;
    sent = throttle_emit_remote(u, (uint32)allowed_bytes);
    u->remote_q.budget -= (int32)sent * 1000;
    return sent;
}

/* --- inbound traffic logging ----------------------------------------- */

static const char *trace_dir_label(int dir)
{
    switch (dir) {
    case PAD_TRACE_DTE:    return "CLIENT";
    case PAD_TRACE_REMOTE: return "SERVICE";
    default:               return "?";
    }
}

/* hexdump -C row: 8-char offset, 2 spaces, two halves of 8 bytes
   ("XX " each), 2-space gap between halves, then "  |" and the
   printable ASCII column, then "|\n". Missing bytes in a short final
   row are padded with three spaces so column alignment is preserved. */
static void trace_hexdump_row(FILE *f, uint32 offset,
                              const uint8 *data, uint32 row_len)
{
    uint32 j;
    fprintf(f, "%08x  ", (unsigned)offset);
    for (j = 0; j < 8; j++) {
        if (j < row_len) fprintf(f, "%02x ", data[j]);
        else             fprintf(f, "   ");
    }
    fputc(' ', f);
    for (j = 8; j < 16; j++) {
        if (j < row_len) fprintf(f, "%02x ", data[j]);
        else             fprintf(f, "   ");
    }
    fputs(" |", f);
    for (j = 0; j < row_len; j++) {
        uint8 c = data[j];
        fputc((c >= 0x20 && c <= 0x7E) ? (int)c : '.', f);
    }
    fputs("|\n", f);
}

static void trace_hexdump(FILE *f, const uint8 *data, uint32 len)
{
    uint32 row;
    for (row = 0; row < len; row += 16) {
        uint32 row_len = len - row;
        if (row_len > 16) row_len = 16;
        trace_hexdump_row(f, row, data + row, row_len);
    }
    fprintf(f, "%08x\n", (unsigned)len);
}

/* Write one entry: timestamp + direction header, then the hexdump. */
static void trace_emit(user_session_t *u, int direction,
                       const uint8 *buf, uint32 len)
{
    struct timespec ts;
    struct tm       tm;
    char            tsbuf[32];
    if (u->trace_fp == NULL || len == 0) return;
    clock_gettime(CLOCK_REALTIME, &ts);
    localtime_r(&ts.tv_sec, &tm);
    strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(u->trace_fp, "\n# %s.%03ld %s%s (%u bytes)\n",
            tsbuf, (long)(ts.tv_nsec / 1000000L),
            trace_dir_label(direction),
            g_trace_line_mode ? " [LINE MODE]" : "",
            (unsigned)len);
    trace_hexdump(u->trace_fp, buf, len);
    fflush(u->trace_fp);
}

/* Default mode: single buffer, flush on direction transition. */
static void trace_cb_default(user_session_t *u, int direction,
                             const uint8 *data, uint32 len)
{
    if (u->trace_last_dir != 0 && direction != u->trace_last_dir &&
        u->trace_buf_len > 0) {
        trace_emit(u, u->trace_last_dir, u->trace_buf, u->trace_buf_len);
        u->trace_buf_len = 0;
    }
    u->trace_last_dir = direction;
    while (len > 0) {
        uint32 space = TRACE_BUF - u->trace_buf_len;
        uint32 chunk = (len < space) ? len : space;
        memcpy(u->trace_buf + u->trace_buf_len, data, chunk);
        u->trace_buf_len += chunk;
        data += chunk;
        len  -= chunk;
        if (u->trace_buf_len == TRACE_BUF) {
            trace_emit(u, direction, u->trace_buf, u->trace_buf_len);
            u->trace_buf_len = 0;
        }
    }
}

/* Line mode: independent per-direction buffers. CLIENT accumulates
   until CR; on CR, flush SERVICE buffer (if any) and then CLIENT.
   SERVICE accumulates between CR events; overflow flushes early. */
static void trace_cb_line(user_session_t *u, int direction,
                          const uint8 *data, uint32 len)
{
    if (direction == PAD_TRACE_DTE) {
        uint32 i;
        for (i = 0; i < len; i++) {
            uint8 b = data[i];
            if (u->trace_line_dte_len >= TRACE_BUF) {
                trace_emit(u, PAD_TRACE_DTE,
                           u->trace_line_dte, u->trace_line_dte_len);
                u->trace_line_dte_len = 0;
            }
            u->trace_line_dte[u->trace_line_dte_len++] = b;
            if (b == 0x0D) {
                /* CR: emit SERVICE first (so prompt+echo precedes the
                   logical input it produced), then CLIENT. */
                if (u->trace_line_remote_len > 0) {
                    trace_emit(u, PAD_TRACE_REMOTE,
                               u->trace_line_remote,
                               u->trace_line_remote_len);
                    u->trace_line_remote_len = 0;
                }
                trace_emit(u, PAD_TRACE_DTE,
                           u->trace_line_dte, u->trace_line_dte_len);
                u->trace_line_dte_len = 0;
            }
        }
    } else {
        while (len > 0) {
            uint32 space = TRACE_BUF - u->trace_line_remote_len;
            uint32 chunk = (len < space) ? len : space;
            memcpy(u->trace_line_remote + u->trace_line_remote_len,
                   data, chunk);
            u->trace_line_remote_len += chunk;
            data += chunk;
            len  -= chunk;
            if (u->trace_line_remote_len == TRACE_BUF) {
                trace_emit(u, PAD_TRACE_REMOTE,
                           u->trace_line_remote,
                           u->trace_line_remote_len);
                u->trace_line_remote_len = 0;
            }
        }
    }
}

static void trace_callback(void *ctx, int direction,
                           const uint8 *data, uint32 len)
{
    user_session_t *u = (user_session_t *)ctx;
    if (u == NULL || u->trace_fp == NULL || len == 0) return;
    if (g_trace_line_mode) trace_cb_line(u, direction, data, len);
    else                   trace_cb_default(u, direction, data, len);
}

static void trace_open(user_session_t *u, int is_stdin)
{
    char            path[256];
    struct timespec ts;
    struct tm       tm;
    char            opened[32];
    if (!g_trace_enabled) return;
    g_trace_seq++;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(path, sizeof(path), "%s-%ld-%d.log",
             g_trace_prefix, (long)ts.tv_sec, g_trace_seq);
    u->trace_fp = fopen(path, "w");
    if (u->trace_fp == NULL) {
        fprintf(stderr, "trace: failed to open %s: %s\n",
                path, strerror(errno));
        return;
    }
    localtime_r(&ts.tv_sec, &tm);
    strftime(opened, sizeof(opened), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(u->trace_fp,
            "# Padawan-Lite trace log (session %d, %s)\n"
            "# opened: %s.%03ld\n"
            "# inbound traffic only: CLIENT = user -> PAD, "
            "SERVICE = host -> PAD\n"
            "# format: hexdump -C; mode: %s\n",
            g_trace_seq, is_stdin ? "stdin" : "tcp",
            opened, (long)(ts.tv_nsec / 1000000L),
            g_trace_line_mode
                ? "line (CLIENT flushes on CR, paired with SERVICE)"
                : "per-direction-transition");
    u->trace_last_dir = 0;
    u->trace_buf_len  = 0;
    u->trace_line_dte_len    = 0;
    u->trace_line_remote_len = 0;
}

static void trace_close(user_session_t *u)
{
    if (u->trace_fp == NULL) return;
    if (g_trace_line_mode) {
        if (u->trace_line_remote_len > 0) {
            trace_emit(u, PAD_TRACE_REMOTE,
                       u->trace_line_remote, u->trace_line_remote_len);
            u->trace_line_remote_len = 0;
        }
        if (u->trace_line_dte_len > 0) {
            trace_emit(u, PAD_TRACE_DTE,
                       u->trace_line_dte, u->trace_line_dte_len);
            u->trace_line_dte_len = 0;
        }
    } else if (u->trace_buf_len > 0 && u->trace_last_dir != 0) {
        trace_emit(u, u->trace_last_dir, u->trace_buf, u->trace_buf_len);
        u->trace_buf_len = 0;
    }
    fclose(u->trace_fp);
    u->trace_fp = NULL;
}

/* --- PAD callbacks (ctx is the user_session_t *) --------------------- */

static void cb_dte(void *ctx, const uint8 *data, uint32 len)
{
    user_session_t *u = (user_session_t *)ctx;
    if (u == NULL || u->write_fd < 0 || len == 0) return;
    if (g_throttle_bps > 0) {
        throttle_enqueue(&u->dte_q, data, len);
        throttle_drain_dte(u);
        return;
    }
    if (u->is_stdin) {
        /* Stdin user: raw write, no IAC escaping (terminal is not a
           telnet client). */
        (void)!write(u->write_fd, data, (size_t)len);
    } else {
        /* TCP user (possibly a telnet client): escape literal 0xFF
           bytes per RFC 854. */
        user_telnet_write(u->write_fd, data, len);
    }
}

static void cb_remote(void *ctx, const uint8 *data, uint32 len)
{
    user_session_t *u = (user_session_t *)ctx;
    if (u == NULL) return;
    if (g_throttle_bps > 0) {
        throttle_enqueue(&u->remote_q, data, len);
        throttle_drain_remote(u);
        return;
    }
    /* Use the call handle that Padawan-Lite's SELECTION dispatch populated
       inside the session, not a separate one. The two were divergent
       in v1.1 before this fix: a stale u->call with call_id = 0 was
       being looked up, which after the generation-counter change
       (slot 0's gen becomes 1 on first use) failed to match any slot
       and silently dropped outbound data. */
    (void)x25_send(&u->pad.call, data, len, 0);
}

/* --- session setup ---------------------------------------------------- */

static void setup_stdin_session(uint8 profile_id)
{
    user_session_t *u = alloc_session();
    if (u == NULL) return;
    u->is_stdin = 1;
    u->read_fd  = STDIN_FILENO;
    u->write_fd = STDOUT_FILENO;

    enter_raw_mode();
    if (g_termios_saved) {
        if (pad_init_handshake(&u->pad, profile_id,
                               cb_dte, cb_remote, u) != 0) {
            destroy_session(u);
            return;
        }
        pad_set_identification(&u->pad, "PADAWAN-LITE v1.1");
        if (g_auth_active) {
            pad_set_auth_callback(&u->pad, nui_check_cb, NULL);
        }
        if (g_telnet_defaults) apply_telnet_defaults(&u->pad);
        trace_open(u, 1);
        if (u->trace_fp != NULL) {
            pad_set_trace_callback(&u->pad, trace_callback, u);
        }
        signal(SIGWINCH, on_winch);
        push_window_size();
        fprintf(stderr,
                "Padawan-Lite v1.1 - profile %u (simple).\n"
                "Address = TCP port on localhost (override via -c map).\n"
                "Press Enter to begin. Ctrl-B = break, Ctrl-P = recall, "
                "Ctrl-D = exit.\n",
                (unsigned)profile_id);
    } else {
        if (pad_init(&u->pad, profile_id, cb_dte, cb_remote, u) != 0) {
            destroy_session(u);
            return;
        }
        if (g_auth_active) {
            pad_set_auth_callback(&u->pad, nui_check_cb, NULL);
        }
        if (g_telnet_defaults) apply_telnet_defaults(&u->pad);
        trace_open(u, 1);
        if (u->trace_fp != NULL) {
            pad_set_trace_callback(&u->pad, trace_callback, u);
        }
        fprintf(stderr,
                "Padawan-Lite v1.1 (non-interactive).\n");
    }
    fflush(stderr);

    /* Make the next x25_call associate the call with this session. */
    x25_bridge_bind(&u->pad);
}

static int start_listener(int port)
{
    int fd;
    int one = 1;
    struct sockaddr_in addr;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((unsigned short)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }
    {
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    return fd;
}

static void accept_session(uint8 profile_id)
{
    int fd;
    int flags;
    user_session_t *u;

    fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) {
        if (errno != EAGAIN && errno != EINTR) {
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
        }
        return;
    }

    u = alloc_session();
    if (u == NULL) {
        static const char busy[] = "Padawan-Lite: session table full.\r\n";
        (void)!write(fd, busy, sizeof(busy) - 1);
        close(fd);
        return;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    u->is_stdin = 0;
    u->read_fd  = fd;
    u->write_fd = fd;

    user_telnet_init(&u->telnet, fd);
    user_telnet_send_initial(&u->telnet);

    if (pad_init_handshake(&u->pad, profile_id,
                           cb_dte, cb_remote, u) != 0) {
        destroy_session(u);
        return;
    }
    pad_set_identification(&u->pad, "PADAWAN-LITE v1.1");
    if (g_auth_active) {
        pad_set_auth_callback(&u->pad, nui_check_cb, NULL);
    }
    if (g_telnet_defaults) apply_telnet_defaults(&u->pad);
    trace_open(u, 0);
    if (u->trace_fp != NULL) {
        pad_set_trace_callback(&u->pad, trace_callback, u);
    }

    /* Make the next x25_call from this session associate correctly. */
    x25_bridge_bind(&u->pad);
}

/* --- I/O per session ------------------------------------------------- */

static void handle_user_input(user_session_t *u)
{
    uint8 buf[256];
    ssize_t n;
    uint32 i;
    uint32 nn;

    n = read(u->read_fd, buf, sizeof(buf));
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        destroy_session(u);
        return;
    }
    if (n == 0) {
        /* EOF: stdin session exits the program; TCP session just goes away. */
        if (u->is_stdin) {
            destroy_session(u);
        } else {
            destroy_session(u);
        }
        return;
    }
    nn = (uint32)n;

    /* Bind so any x25_call triggered by this byte stream associates with
       the right session. */
    x25_bridge_bind(&u->pad);

    if (u->is_stdin && g_termios_saved) {
        /* Interactive-tty intercepts: Ctrl-D = quit; Ctrl-B = break. */
        for (i = 0; i < nn; i++) {
            uint8 b = buf[i];
            if (b == 0x04) {                /* Ctrl-D */
                destroy_session(u);
                return;
            }
            if (b == 0x02) {                /* Ctrl-B */
                pad_break(&u->pad);
                continue;
            }
            pad_input_dte(&u->pad, &b, 1);
        }
    } else if (u->is_stdin) {
        /* Piped stdin: no IAC filtering, transparent byte stream. */
        pad_input_dte(&u->pad, buf, nn);
    } else {
        /* TCP user session: filter IAC sequences out before the PAD
           sees them; the filter also handles negotiation responses
           and SB parsing (incl. NAWS capture). */
        uint8  filtered[256];
        uint16 w = 0, h = 0;
        uint32 n_filtered = user_telnet_filter(&u->telnet,
                                               buf, nn, filtered);
        /* If this user sent NAWS, forward it to their bridge call only
           (per-session, so other users' sizes are untouched). Consume
           the flag so we don't re-send on every byte. */
        if (user_telnet_get_naws(&u->telnet, &w, &h)) {
            x25_bridge_set_window_size_for_session(&u->pad, w, h);
            u->telnet.has_naws = 0;
        }
        if (n_filtered > 0) {
            pad_input_dte(&u->pad, filtered, n_filtered);
        }
    }
}

/* --- main loop -------------------------------------------------------- */

static int any_session_active(void)
{
    int i;
    for (i = 0; i < MAX_SESSIONS; i++) {
        if (g_sessions[i].in_use) return 1;
    }
    return 0;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -c, --config <file>      load address->host:port map\n"
        "  -l, --listen <port>      accept TCP user connections (multi-session)\n"
        "  -a, --auth <file>        require NUI (file: one allowed NUI per line)\n"
        "  -t, --telnet-defaults    SET 2:0, 3:0, 4:1 (Telnet-friendly)\n",
        argv0);
    fprintf(stderr,
        "  -b, --baud <bps>         throttle I/O to <bps> bits/sec"
                                 " (300, 1200, 2400, ...; 0 = off)\n"
        "      --trace              log inbound traffic per session"
                                 " (default prefix: " TRACE_DEFAULT_PREFIX ")\n"
        "      --trace-prefix <p>   override --trace filename prefix"
                                 " (implies --trace)\n"
        "      --trace-line-mode    accumulate CLIENT bytes until CR;"
                                 " pair with SERVICE (implies --trace)\n");
    fprintf(stderr,
        "      --pcp-port <port>    PAD Control Protocol listener"
                                 " (localhost; 0 = off)\n"
        "  -h, --help               this help\n"
        "  Default: single session over stdin/stdout.\n");
}

int main(int argc, char **argv)
{
    const char *map_file = NULL;
    int listen_port = 0;
    int ai;
    uint8 profile_id = X3_PROFILE_SIMPLE;
    uint32 last_ms;

    for (ai = 1; ai < argc; ai++) {
        if (strcmp(argv[ai], "-c") == 0 ||
            strcmp(argv[ai], "--config") == 0) {
            if (++ai >= argc) { usage(argv[0]); return 2; }
            map_file = argv[ai];
        } else if (strcmp(argv[ai], "-l") == 0 ||
                   strcmp(argv[ai], "--listen") == 0) {
            if (++ai >= argc) { usage(argv[0]); return 2; }
            listen_port = atoi(argv[ai]);
            if (listen_port < 1 || listen_port > 65535) {
                usage(argv[0]); return 2;
            }
        } else if (strcmp(argv[ai], "-a") == 0 ||
                   strcmp(argv[ai], "--auth") == 0) {
            if (++ai >= argc) { usage(argv[0]); return 2; }
            if (load_nui_file(argv[ai]) != 0) {
                fprintf(stderr, "failed to load auth file %s: %s\n",
                        argv[ai], strerror(errno));
                return 1;
            }
            if (g_nui_count == 0) {
                fprintf(stderr,
                        "auth file %s contained no valid NUIs\n", argv[ai]);
                return 1;
            }
            g_auth_active = 1;
        } else if (strcmp(argv[ai], "-t") == 0 ||
                   strcmp(argv[ai], "--telnet-defaults") == 0) {
            g_telnet_defaults = 1;
        } else if (strcmp(argv[ai], "-b") == 0 ||
                   strcmp(argv[ai], "--baud") == 0) {
            long bps;
            if (++ai >= argc) { usage(argv[0]); return 2; }
            bps = atol(argv[ai]);
            if (bps < 0 || bps > 921600L) { usage(argv[0]); return 2; }
            if (bps > 0) {
                /* 10 bits per byte: 1 start + 8 data + 1 stop. */
                g_throttle_bps = (int32)(bps / 10);
                if (g_throttle_bps < 1) g_throttle_bps = 1;
                /* Burst cap: 250 ms worth of credit so a quiet session
                   doesn't sit idle when activity resumes, but bursts
                   stay bounded. Floor at 1 byte for very low rates. */
                g_throttle_burst_mb = (g_throttle_bps * 1000) / 4;
                if (g_throttle_burst_mb < 1000) g_throttle_burst_mb = 1000;
            }
        } else if (strcmp(argv[ai], "--trace") == 0) {
            g_trace_enabled = 1;
        } else if (strcmp(argv[ai], "--trace-line-mode") == 0) {
            g_trace_line_mode = 1;
            g_trace_enabled   = 1;
        } else if (strcmp(argv[ai], "--trace-prefix") == 0) {
            size_t n;
            if (++ai >= argc) { usage(argv[0]); return 2; }
            n = strlen(argv[ai]);
            if (n == 0 || n > TRACE_PREFIX_MAX) { usage(argv[0]); return 2; }
            memcpy(g_trace_prefix, argv[ai], n);
            g_trace_prefix[n] = '\0';
            g_trace_enabled   = 1;
        } else if (strcmp(argv[ai], "--pcp-port") == 0) {
            int port;
            if (++ai >= argc) { usage(argv[0]); return 2; }
            port = atoi(argv[ai]);
            if (port < 0 || port > 65535) { usage(argv[0]); return 2; }
            g_pcp_port = port;
        } else if (strcmp(argv[ai], "-h") == 0 ||
                   strcmp(argv[ai], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    if (x25_init() != X25_OK) {
        fprintf(stderr, "x25_init failed\n");
        return 1;
    }
    if (map_file != NULL) {
        if (x25_bridge_load_map(map_file) != 0) {
            fprintf(stderr, "warning: failed to load address map %s: %s\n",
                    map_file, strerror(errno));
        }
    }

    if (listen_port > 0) {
        g_listen_fd = start_listener(listen_port);
        if (g_listen_fd < 0) {
            fprintf(stderr, "listen on port %d failed: %s\n",
                    listen_port, strerror(errno));
            return 1;
        }
        fprintf(stderr,
                "Padawan-Lite v1.1 - listening on TCP port %d "
                "(MAX_SESSIONS = %d).\n",
                listen_port, MAX_SESSIONS);
        if (g_auth_active) {
            fprintf(stderr,
                    "NUI auth: %d entries loaded; calls require a "
                    "matching N facility.\n", g_nui_count);
        }
        if (g_telnet_defaults) {
            fprintf(stderr,
                    "Telnet-friendly defaults: SET 2:0, 3:0, 4:1 "
                    "applied per session.\n");
        }
        if (g_throttle_bps > 0) {
            fprintf(stderr,
                    "Throttle: %ld bytes/sec per direction per session.\n",
                    (long)g_throttle_bps);
        }
        if (g_trace_enabled) {
            fprintf(stderr,
                    "Trace: inbound traffic logged to "
                    "%s-<ts>-<n>.log (%s)\n",
                    g_trace_prefix,
                    g_trace_line_mode ? "line mode" : "default mode");
        }
        fflush(stderr);
    } else {
        setup_stdin_session(profile_id);
        if (!any_session_active()) return 1;
    }

    if (g_pcp_port > 0) {
        if (pcp_init(g_pcp_port) != 0) {
            fprintf(stderr, "PCP listen on 127.0.0.1:%d failed: %s\n",
                    g_pcp_port, strerror(errno));
            return 1;
        }
        fprintf(stderr,
                "PCP: control protocol listening on 127.0.0.1:%d\n",
                g_pcp_port);
        fflush(stderr);
    }

    last_ms = now_ms();
    for (;;) {
        /* Capacity: 1 user-listener + 1 PCP-listener
           + 2 fds per user session (user fd + bridge fd)
           + PCP_MAX_CONNS PCP control connections. */
        struct pollfd pfds[2 + MAX_SESSIONS * 2 + PCP_MAX_CONNS];
        int           nfds = 0;
        int           idx_listen = -1;
        int           idx_pcp_listen = -1;
        int           idx_pcp_first  = -1;
        int           pcp_count      = 0;
        int           idx_user[MAX_SESSIONS];
        int           idx_bridge[MAX_SESSIONS];
        int           i;
        int           rc;
        uint32        cur_ms;
        uint32        elapsed_ms;
        int           any_ticking = 0;
        int           timeout;

        if (g_winch_pending) {
            g_winch_pending = 0;
            push_window_size();
        }

        for (i = 0; i < MAX_SESSIONS; i++) {
            idx_user[i] = -1;
            idx_bridge[i] = -1;
        }

        if (g_listen_fd >= 0) {
            pfds[nfds].fd      = g_listen_fd;
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            idx_listen = nfds++;
        }
        if (pcp_enabled()) {
            pfds[nfds].fd      = pcp_listener_fd();
            pfds[nfds].events  = POLLIN;
            pfds[nfds].revents = 0;
            idx_pcp_listen = nfds++;
        }

        for (i = 0; i < MAX_SESSIONS; i++) {
            if (!g_sessions[i].in_use) continue;
            if (g_sessions[i].read_fd >= 0) {
                /* Throttle backpressure: stop accepting user input while
                   the upstream queue is near full. POLLHUP/POLLERR are
                   still delivered with events = 0. */
                short ev = POLLIN;
                if (g_throttle_bps > 0 &&
                    g_sessions[i].remote_q.len >= THROTTLE_HIGH_WATER) {
                    ev = 0;
                }
                pfds[nfds].fd      = g_sessions[i].read_fd;
                pfds[nfds].events  = ev;
                pfds[nfds].revents = 0;
                idx_user[i] = nfds++;
            }
            {
                int bfd = x25_bridge_fd_for_session(&g_sessions[i].pad);
                if (bfd >= 0) {
                    /* Same idea on the bridge side: suppress POLLIN when
                       the downstream queue is near full, leaving POLLOUT
                       for connect / write-buffer completion. */
                    short ev = POLLOUT;
                    if (g_throttle_bps == 0 ||
                        g_sessions[i].dte_q.len < THROTTLE_HIGH_WATER) {
                        ev |= POLLIN;
                    }
                    pfds[nfds].fd      = bfd;
                    pfds[nfds].events  = ev;
                    pfds[nfds].revents = 0;
                    idx_bridge[i] = nfds++;
                }
            }
            if (g_sessions[i].pad.state == PAD_STATE_DATA_TRANSFER &&
                g_sessions[i].pad.asm_len > 0 &&
                g_sessions[i].pad.params.values[X3_PAR_IDLE] != 0) {
                any_ticking = 1;
            }
            if (g_throttle_bps > 0 &&
                (g_sessions[i].dte_q.len > 0 ||
                 g_sessions[i].remote_q.len > 0)) {
                any_ticking = 1;
            }
        }

        if (pcp_enabled()) {
            int got = pcp_collect_pollfds(&pfds[nfds],
                                          PCP_MAX_CONNS);
            if (got > 0) {
                idx_pcp_first = nfds;
                pcp_count     = got;
                nfds         += got;
            }
        }

        if (nfds == 0) break; /* no sessions and no listener */

        timeout = any_ticking ? 50 : -1;
        rc = poll(pfds, (nfds_t)nfds, timeout);
        if (rc < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        cur_ms = now_ms();
        elapsed_ms = cur_ms - last_ms;
        if (elapsed_ms >= 50) {
            uint32 ticks = elapsed_ms / 50;
            for (i = 0; i < MAX_SESSIONS; i++) {
                if (!g_sessions[i].in_use) continue;
                pad_tick(&g_sessions[i].pad, ticks);
                if (g_throttle_bps > 0) {
                    throttle_refill(&g_sessions[i].dte_q,    (int32)elapsed_ms);
                    throttle_refill(&g_sessions[i].remote_q, (int32)elapsed_ms);
                    throttle_drain_dte(&g_sessions[i]);
                    throttle_drain_remote(&g_sessions[i]);
                }
            }
            last_ms = cur_ms;
        }

        if (rc == 0) continue;

        if (idx_listen >= 0 && pfds[idx_listen].revents) {
            accept_session(profile_id);
        }
        if (idx_pcp_listen >= 0 && pfds[idx_pcp_listen].revents) {
            pcp_handle_accept();
        }
        if (idx_pcp_first >= 0) {
            int k;
            for (k = 0; k < pcp_count; k++) {
                struct pollfd *pf = &pfds[idx_pcp_first + k];
                if (pf->revents) {
                    pcp_handle_conn(pf->fd, pf->revents);
                }
            }
        }

        for (i = 0; i < MAX_SESSIONS; i++) {
            if (idx_bridge[i] >= 0 &&
                g_sessions[i].in_use &&
                pfds[idx_bridge[i]].revents) {
                int bfd = pfds[idx_bridge[i]].fd;
                x25_bridge_bind(&g_sessions[i].pad);
                x25_bridge_poll_fd(bfd, pfds[idx_bridge[i]].revents);
            }
            if (idx_user[i] >= 0 &&
                g_sessions[i].in_use &&
                pfds[idx_user[i]].revents) {
                handle_user_input(&g_sessions[i]);
            }
        }

        /* If running in stdin-only mode and that session is gone, quit. */
        if (g_listen_fd < 0 && !any_session_active()) break;
    }

    /* Flush + clean up all sessions. */
    {
        int i;
        for (i = 0; i < MAX_SESSIONS; i++) {
            if (g_sessions[i].in_use) {
                pad_flush(&g_sessions[i].pad);
                destroy_session(&g_sessions[i]);
            }
        }
    }
    if (g_listen_fd >= 0) close(g_listen_fd);
    pcp_shutdown();
    fprintf(stderr, "\n[padawan-lite shutdown]\n");
    return 0;
}
