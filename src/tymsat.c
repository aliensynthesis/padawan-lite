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

/* Stand-alone TYMSAT state machine. See include/tymsat.h for the
   architectural rationale, the source citations ([HTU82] / [NPCF85])
   and the scope boundaries. */

#include <string.h>
#include <ctype.h>

#include "tymsat.h"

#define TYMSAT_CTL_H  0x08   /* ^H, half duplex */
#define TYMSAT_CTL_P  0x10   /* ^P, even parity */
#define TYMSAT_CTL_R  0x12   /* ^R, terminal-controlled inbound flow */
#define TYMSAT_CTL_X  0x18   /* ^X, network-controlled outbound flow */

#define TYMSAT_CR  0x0D
#define TYMSAT_LF  0x0A

/* Line assembly scratch. The longest thing we build is a message line
   plus framing; the catalogue's longest entry is "requested subprocess
   is unavailable at this time" at 47 characters. */
#define TYMSAT_LINE_BUF  128

/* --- terminal identifier table --------------------------------------

   [HTU82:127-137]. Order follows the pamphlet's own table. Note that
   'H' is absent from the identifier set -- the pamphlet lists
   A,B,C,D,E,F,G,I,P with no H -- which is convenient, since ^H is a
   login control character.

   RECORD-ONLY per include/tymsat.h: these fields describe the
   identifier, they do not configure the session. */
static const tymsat_tid_t TID_TABLE[] = {
    /* id,  cps, cps_alt, ebcd, needs_cr, cr_delay, description */
    { 'A',   30,     120,    0,        0,        0,
      "CRT terminals; personal computers" },
    { 'B',   15,       0,    0,        0,        0,
      "all terminals" },
    { 'C',   30,       0,    0,        0,        0,
      "impact printers" },
    { 'D',   10,       0,    0,        0,        0,
      "all terminals" },
    { 'E',   30,       0,    0,        0,        1,
      "thermal printers" },
    { 'F',   15,      30,    0,        0,        0,
      "BETA terminals (15cps in / 30cps out)" },
    { 'G',   30,     120,    0,        0,        0,
      "belt printers; G.E. Terminet" },
    { 'I',  120,       0,    0,        0,        1,
      "matrix printers" },
    /* [HTU82:137] renders this row "P + carriage return", the only
       identifier requiring a terminator, and the only non-ASCII one
       (EBCD/Correspondence, Selectric-type terminals such as the
       2741). Its 14.8cps is stored truncated to 14. */
    { 'P',   14,       0,    1,        1,        0,
      "Selectric-type terminals (EBCD/Correspondence, 14.8cps)" }
};

#define TID_TABLE_LEN  (sizeof(TID_TABLE) / sizeof(TID_TABLE[0]))

/* --- message catalogue ----------------------------------------------

   [HTU82:196-289], stored in the canonical lowercase form; the
   uppercase_messages setting is applied at emit time. Indices MUST
   track tymsat_msg_t exactly -- a compile-time guard below checks the
   count.

   VERIFY: the pamphlet's MESSAGES section renders every entry
   uppercase and its procedural walkthrough renders the prompts
   lowercase; see the uppercase_messages commentary in
   include/tymsat.h. The wording here is the pamphlet's, verbatim
   apart from case. */
static const char *const MSG_TEXT[TYMSAT_MSG_COUNT] = {
    "access not permitted",                          /* [HTU82:198] */
    "all ports busy",                                /* [HTU82:201] */
    "bad host number",                               /* [HTU82:204] */
    "bad mud",                                       /* [HTU82:207] */
    "circuits busy",                                 /* [HTU82:210] */
    "data lost toward host",                         /* [HTU82:213] */
    "data lost toward terminal",                     /* [HTU82:216] */
    "dropped by host system",                        /* [HTU82:219] */
    "error, type user name",                         /* [HTU82:222] */
    "error, type password",                          /* [HTU82:225] */
    "host down",                                     /* [HTU82:228] */
    "host is online",                                /* [HTU82:231] */
    "host not available thru net",                   /* [HTU82:234] */
    "host not responding",                           /* [HTU82:237] */
    "host shut",                                     /* [HTU82:240] */
    "logon aborted... disconnecting",                /* [HTU82:243] */
    "no host specified",                             /* [HTU82:246] */
    "no path available... disconnecting",            /* [HTU82:249] */
    "out of channels",                               /* [HTU82:252] */
    "password:",                                     /* [HTU82:255] */
    "please log in:",                                /* [HTU82:258] */
    "pls see your rep",                              /* [HTU82:261] */
    "please try again",                              /* [HTU82:264] */
    "re-enter address and data",                     /* [HTU82:267] */
    "requested subprocess is unavailable at this time", /* [HTU82:270] */
    "ring no answer on port #",                      /* [HTU82:273] */
    "system error on port #",                        /* [HTU82:276] */
    "temporary network problem",                     /* [HTU82:279] */
    "try again in 2 minutes",                        /* [HTU82:282] */
    "type \"p\" ahead of address",                   /* [HTU82:285] */
    "user name",                                     /* [HTU82:288] */
    "please type your terminal identifier",           /* [HTU82:28]  */
    /* Not from the pamphlet: observed from a 1986 TYMNET client. */
    "call connected"
};

/* Compile-time guard: MSG_TEXT must have exactly TYMSAT_MSG_COUNT
   entries. A mismatch makes this array size zero or negative, which
   is a constraint violation in C89. */
typedef char tymsat_msg_table_size_check[
    (sizeof(MSG_TEXT) / sizeof(MSG_TEXT[0])) == TYMSAT_MSG_COUNT ? 1 : -1];

/* --- small helpers --------------------------------------------------- */

static void emit_dte_bytes(tymsat_session_t *s, const char *text, uint32 len)
{
    if (s->emit_dte != NULL && len > 0) {
        s->emit_dte(s->ctx, (const uint8 *)text, len);
    }
}

static void emit_dte_str(tymsat_session_t *s, const char *text)
{
    emit_dte_bytes(s, text, (uint32)strlen(text));
}

/* Line terminator toward the terminal.

   VERIFY: neither source states the framing around service messages.
   CR LF is the conventional start-stop pairing and is what the PAD
   core emits, so we match it for consistency. */
static void emit_crlf(tymsat_session_t *s)
{
    static const char crlf[2] = { TYMSAT_CR, TYMSAT_LF };
    emit_dte_bytes(s, crlf, 2);
}

/* Append an unsigned decimal to buf at *pos. C89 has no snprintf, and
   sprintf's return value is not portable enough to lean on, so this
   is done by hand. Caller guarantees room (5 digits max for uint16). */
static void append_uint(char *buf, uint32 *pos, uint16 value)
{
    char  digits[6];
    int   n = 0;

    if (value == 0) {
        buf[(*pos)++] = '0';
        return;
    }
    while (value > 0 && n < (int)sizeof(digits)) {
        digits[n++] = (char)('0' + (value % 10));
        value = (uint16)(value / 10);
    }
    while (n > 0) {
        buf[(*pos)++] = digits[--n];
    }
}

/* Emit catalogue text applying the configured case. */
static void emit_message_text(tymsat_session_t *s, const char *text)
{
    char   buf[TYMSAT_LINE_BUF];
    uint32 i;
    uint32 len;

    len = (uint32)strlen(text);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;

    if (s->cfg != NULL && s->cfg->uppercase_messages) {
        for (i = 0; i < len; i++) {
            buf[i] = (char)toupper((unsigned char)text[i]);
        }
        emit_dte_bytes(s, buf, len);
    } else {
        emit_dte_bytes(s, text, len);
    }
}

const char *tymsat_message_text(tymsat_msg_t msg)
{
    if ((int)msg < 0 || (int)msg >= (int)TYMSAT_MSG_COUNT) return NULL;
    return MSG_TEXT[msg];
}

void tymsat_emit_message(tymsat_session_t *s, tymsat_msg_t msg)
{
    const char *text;

    if (s == NULL) return;
    text = tymsat_message_text(msg);
    if (text == NULL) return;

    emit_crlf(s);
    emit_message_text(s, text);
    emit_crlf(s);
}

const tymsat_tid_t *tymsat_tid_by_char(uint8 c)
{
    uint16 i;
    int    up;

    up = toupper((unsigned char)c);
    for (i = 0; i < (uint16)TID_TABLE_LEN; i++) {
        if ((int)TID_TABLE[i].id == up) return &TID_TABLE[i];
    }
    return NULL;
}

const tymsat_tid_t *tymsat_tid_table(uint16 *count_out)
{
    if (count_out != NULL) *count_out = (uint16)TID_TABLE_LEN;
    return TID_TABLE;
}

/* --- prompts ---------------------------------------------------------- */

/* The node/port line that precedes the login prompt, [HTU82:34-37]:

       -NNNN-PPP-
       please log in:

   and the three-number WATS variant of [HTU82:84], -NNNN-SS-PPP-.
   See the VERIFY notes on tymsat_config_t regarding field widths. */
static void emit_node_port_line(tymsat_session_t *s)
{
    char   buf[TYMSAT_LINE_BUF];
    uint32 pos = 0;

    buf[pos++] = '-';
    append_uint(buf, &pos, s->cfg->node_number);
    buf[pos++] = '-';
    if (s->cfg->emit_slot_number) {
        append_uint(buf, &pos, s->cfg->slot_number);
        buf[pos++] = '-';
    }
    append_uint(buf, &pos, s->cfg->port_number);
    buf[pos++] = '-';

    emit_crlf(s);
    emit_dte_bytes(s, buf, pos);
}

/* Prompts that expect input on the same line are emitted WITHOUT a
   trailing terminator: [HTU82:256] "Type your password on the same
   line as the prompt."

   VERIFY: neither source shows whether a space follows the colon. We
   emit none. */
static void emit_prompt(tymsat_session_t *s, tymsat_msg_t msg)
{
    const char *text = tymsat_message_text(msg);
    if (text == NULL) return;
    emit_crlf(s);
    emit_message_text(s, text);
}

/* Full "-NNNN-PPP- / please log in:" sequence, used both at the start
   of a session and on return from a cleared circuit. */
static void emit_login_prompt(tymsat_session_t *s)
{
    emit_node_port_line(s);
    emit_prompt(s, TYMSAT_MSG_PLEASE_LOG_IN);
}

/* Reset the per-login accumulators, keeping the terminal identifier:
   [HTU82:57-61] a logoff returns the user to "please log in:" without
   dropping carrier, and the TID is not requested a second time. */
static void reset_login_state(tymsat_session_t *s)
{
    s->login_buf[0]   = '\0';
    s->login_len      = 0;
    s->username[0]    = '\0';
    s->host_number[0] = '\0';
    s->password[0]    = '\0';
    s->password_len   = 0;
    s->user           = NULL;
    s->login_ticks    = 0;
    s->ctl_flags      = 0;
    s->pending_len    = 0;
    s->host_flow_held = 0;
    s->terminal_flow_held = 0;
}

/* --- login parsing ---------------------------------------------------- */

/* Case-insensitive comparison for usernames.

   VERIFY: neither source states whether usernames are case-sensitive.
   Period practice and the pamphlet's all-caps rendering
   ("INFORMATION", [HTU82:7]) suggest they were treated as uppercase;
   we match case-insensitively, which accepts both. */
static int name_equal(const char *a, const char *b)
{
    while (*a != '\0' && *b != '\0') {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return 0;
        a++;
        b++;
    }
    return (*a == '\0' && *b == '\0');
}

static const tymsat_user_t *find_user(const tymsat_config_t *cfg,
                                      const char *username)
{
    uint16 i;

    if (cfg == NULL || cfg->users == NULL) return NULL;
    for (i = 0; i < cfg->user_count; i++) {
        if (name_equal(cfg->users[i].username, username)) {
            return &cfg->users[i];
        }
    }
    return NULL;
}

/* Split the accumulated login string into username and host number on
   the colon, per [NPCF85 p. 14-6]. See the LOGIN STRING GRAMMAR note
   in include/tymsat.h for why the colon and not the semicolon. */
static void split_login_string(tymsat_session_t *s)
{
    const char *colon;
    uint32      name_len;
    uint32      host_len;

    s->username[0]    = '\0';
    s->host_number[0] = '\0';

    colon = strchr(s->login_buf, ':');
    if (colon == NULL) {
        name_len = (uint32)strlen(s->login_buf);
        if (name_len > TYMSAT_USERNAME_MAX) name_len = TYMSAT_USERNAME_MAX;
        memcpy(s->username, s->login_buf, name_len);
        s->username[name_len] = '\0';
        return;
    }

    name_len = (uint32)(colon - s->login_buf);
    if (name_len > TYMSAT_USERNAME_MAX) name_len = TYMSAT_USERNAME_MAX;
    memcpy(s->username, s->login_buf, name_len);
    s->username[name_len] = '\0';

    host_len = (uint32)strlen(colon + 1);
    if (host_len > TYMSAT_HOSTNUM_MAX) host_len = TYMSAT_HOSTNUM_MAX;
    memcpy(s->host_number, colon + 1, host_len);
    s->host_number[host_len] = '\0';
}

/* --- circuit establishment -------------------------------------------- */

static void enter_data_transfer(tymsat_session_t *s)
{
    s->state = TYMSAT_STATE_DATA_TRANSFER;

    /* Connect acknowledgement. The default is the client-observed
       "call connected"; the pamphlet's two examples ([HTU82:49-53])
       remain selectable. See tymsat_config_t.accept_msg. */
    emit_crlf(s);
    switch (s->cfg->accept_msg) {
    case TYMSAT_ACCEPT_TERSE:
        /* Emitted literally rather than through the catalogue: a bare
           semicolon has no catalogue entry and no case to fold. */
        emit_dte_str(s, ";");
        break;
    case TYMSAT_ACCEPT_VERBOSE:
        emit_message_text(s, tymsat_message_text(TYMSAT_MSG_HOST_IS_ONLINE));
        break;
    case TYMSAT_ACCEPT_CALL_CONNECTED:
    default:
        emit_message_text(s, tymsat_message_text(TYMSAT_MSG_CALL_CONNECTED));
        break;
    }
    emit_crlf(s);

    /* Replay anything typed while the circuit was building. */
    if (s->pending_len > 0) {
        if (s->emit_remote != NULL) {
            s->emit_remote(s->ctx, s->pending, s->pending_len);
        }
        s->pending_len = 0;
    }
}

/* Return to the login prompt after a failure or a cleared circuit,
   without dropping carrier ([HTU82:57-61]). */
static void return_to_login(tymsat_session_t *s)
{
    reset_login_state(s);
    s->state = TYMSAT_STATE_AWAITING_LOGIN;
    emit_prompt(s, TYMSAT_MSG_PLEASE_LOG_IN);
}

/* Place the call once username, password and destination are settled. */
static void begin_circuit(tymsat_session_t *s)
{
    int rc;

    s->state = TYMSAT_STATE_CIRCUIT_BUILD;
    s->pending_len = 0;

    rc = x25_call(&s->call, s->host_number);
    if (rc == X25_OK) {
        enter_data_transfer(s);
        return;
    }
    if (rc == X25_IN_PROGRESS) {
        return;   /* await tymsat_circuit_connected / _failed */
    }

    /* Immediate failure. Map the transport's cause onto the closest
       catalogue entry; the mapping is documented in deviations.txt. */
    switch (rc) {
    case X25_ERR_NO_ROUTE:
        tymsat_emit_message(s, TYMSAT_MSG_BAD_HOST_NUMBER);
        break;
    case X25_ERR_BUSY:
        tymsat_emit_message(s, TYMSAT_MSG_ALL_PORTS_BUSY);
        break;
    case X25_ERR_REJECTED:
        tymsat_emit_message(s, TYMSAT_MSG_HOST_NOT_AVAILABLE);
        break;
    default:
        tymsat_emit_message(s, TYMSAT_MSG_HOST_DOWN);
        break;
    }
    return_to_login(s);
}

/* Decide the destination and either place the call or complain.
   Called once the username (and password, if any) have been accepted. */
static void resolve_destination_and_call(tymsat_session_t *s)
{
    const tymsat_user_t *u = s->user;

    /* Ignore Host ([NPCF85 p. 14-6]): the typed destination is
       discarded and the configured home is used. */
    if (u->ignore_host) {
        s->host_number[0] = '\0';
    }

    if (s->host_number[0] == '\0') {
        if (u->default_host[0] == '\0') {
            /* [HTU82:246] NO HOST SPECIFIED: "a user name that is
               valid on more than one host, but which is not
               automatically connected to any host by default." */
            tymsat_emit_message(s, TYMSAT_MSG_NO_HOST_SPECIFIED);
            return_to_login(s);
            return;
        }
        strcpy(s->host_number, u->default_host);
    }

    begin_circuit(s);
}

/* --- per-state input handling ----------------------------------------- */

/* AWAITING_TID: exactly one character, [HTU82:30]. 'P' is the sole
   identifier taking a following CR ([HTU82:137]); for it we record the
   identifier and stay here until the CR arrives.

   VERIFY: the pamphlet does not say what a real TYMSAT did with an
   unrecognised identifier character. [HTU82:32] notes the request
   itself may arrive garbled or not at all at mismatched speeds and
   tells the user to "wait a few seconds and type your identifier",
   which implies tolerance rather than an error message. We therefore
   ignore unrecognised characters silently and keep waiting. */
static void feed_awaiting_tid(tymsat_session_t *s, uint8 c)
{
    const tymsat_tid_t *tid;

    if (c == TYMSAT_CR) {
        /* Only meaningful as the terminator for an identifier that
           needs one; a bare CR before any identifier is ignored. */
        if (s->tid != NULL && s->tid->needs_cr) {
            s->login_ticks = 0;
            s->state = TYMSAT_STATE_AWAITING_LOGIN;
            emit_login_prompt(s);
        }
        return;
    }
    if (c == TYMSAT_LF) return;

    tid = tymsat_tid_by_char(c);
    if (tid == NULL) return;

    s->tid = tid;
    if (tid->needs_cr) return;   /* wait for the CR */

    s->login_ticks = 0;
    s->state = TYMSAT_STATE_AWAITING_LOGIN;
    emit_login_prompt(s);
}

/* AWAITING_LOGIN: accumulate the login string, stripping the embedded
   control characters of [HTU82:108-115] into ctl_flags.

   Note that ^H is claimed as a session option here rather than as an
   editing backspace; [HTU82:110] assigns it to half-duplex operation
   and the pamphlet documents no editing keys at all. Recorded as a
   deviation. */
static void feed_awaiting_login(tymsat_session_t *s, uint8 c)
{
    switch (c) {
    case TYMSAT_CTL_H:
        s->ctl_flags |= TYMSAT_CTL_HALF_DUPLEX;
        return;
    case TYMSAT_CTL_P:
        s->ctl_flags |= TYMSAT_CTL_EVEN_PARITY;
        return;
    case TYMSAT_CTL_R:
        s->ctl_flags |= TYMSAT_CTL_TERM_FLOW;
        return;
    case TYMSAT_CTL_X:
        s->ctl_flags |= TYMSAT_CTL_NET_FLOW;
        return;
    case TYMSAT_LF:
        return;
    default:
        break;
    }

    if (c != TYMSAT_CR) {
        if (s->login_len < TYMSAT_LOGIN_BUF) {
            s->login_buf[s->login_len++] = (char)c;
            s->login_buf[s->login_len]   = '\0';
            /* The TYMSAT echoes unless ^H selected half duplex
               ([HTU82:110]). */
            if ((s->ctl_flags & TYMSAT_CTL_HALF_DUPLEX) == 0) {
                emit_dte_bytes(s, (const char *)&c, 1);
            }
        }
        return;
    }

    /* CR: the login string is complete. */
    s->login_ticks = 0;

    if (s->login_len == 0) {
        /* [HTU82:288] USER NAME: "A carriage return or line feed has
           been entered instead of a user name. Type your user name on
           the same line as the prompt."

           That last clause makes the message itself the re-prompt, so
           we emit it in prompt form (no trailing terminator) rather
           than emitting it AND re-issuing "please log in:", which
           would put two prompts on screen for one mistake.

           VERIFY: the pamphlet describes the message and the
           instruction but never shows the resulting screen, so
           "message replaces prompt" is an interpretation. The
           alternative reading (message THEN re-prompt) is equally
           grammatical. */
        emit_prompt(s, TYMSAT_MSG_USER_NAME);
        return;
    }

    split_login_string(s);
    s->login_buf[0] = '\0';
    s->login_len    = 0;

    s->user = find_user(s->cfg, s->username);
    if (s->user == NULL) {
        /* [HTU82:222] ERROR, TYPE USER NAME. The pamphlet's more
           severe ACCESS NOT PERMITTED ([HTU82:198]) covers a barred
           node as well as a bad name, which we cannot distinguish;
           the re-prompting form is the better fit here. */
        tymsat_emit_message(s, TYMSAT_MSG_ERROR_TYPE_USER_NAME);
        emit_prompt(s, TYMSAT_MSG_PLEASE_LOG_IN);
        return;
    }

    /* No Password ([NPCF85 p. 14-6]): skip the prompt entirely. */
    if (s->user->password[0] == '\0') {
        resolve_destination_and_call(s);
        return;
    }

    s->password[0]  = '\0';
    s->password_len = 0;
    s->state = TYMSAT_STATE_AWAITING_PASSWORD;
    emit_prompt(s, TYMSAT_MSG_PASSWORD);
}

/* AWAITING_PASSWORD: never echoed ([HTU82:47]). */
static void feed_awaiting_password(tymsat_session_t *s, uint8 c)
{
    if (c == TYMSAT_LF) return;

    if (c != TYMSAT_CR) {
        if (s->password_len < TYMSAT_PASSWORD_MAX) {
            s->password[s->password_len++] = (char)c;
            s->password[s->password_len]   = '\0';
        }
        return;
    }

    s->login_ticks = 0;

    if (s->password_len == 0) {
        /* [HTU82:255] PASSWORD: "A carriage return or line feed has
           been entered instead of a password. Type your password on
           the same line as the prompt." As with USER NAME above, the
           message doubles as the re-prompt -- and here the two are
           the same string, so emitting both would print "password:"
           twice for one empty entry. */
        emit_prompt(s, TYMSAT_MSG_PASSWORD);
        return;
    }

    if (strcmp(s->password, s->user->password) != 0) {
        /* [HTU82:225] ERROR, TYPE PASSWORD: "Type your correct
           password" -- so we re-prompt for the password rather than
           restarting the whole login. */
        s->password[0]  = '\0';
        s->password_len = 0;
        tymsat_emit_message(s, TYMSAT_MSG_ERROR_TYPE_PASSWORD);
        emit_prompt(s, TYMSAT_MSG_PASSWORD);
        return;
    }

    resolve_destination_and_call(s);
}

/* DATA_TRANSFER: pass through, intercepting XON/XOFF only in the modes
   that [HTU82:113-115] says enable them. */
static void feed_data_transfer(tymsat_session_t *s, const uint8 *data,
                               uint32 len)
{
    uint32 i;
    uint32 run_start = 0;

    for (i = 0; i < len; i++) {
        uint8 c = data[i];
        int   intercept = 0;

        if ((s->ctl_flags & TYMSAT_CTL_TERM_FLOW) != 0) {
            if (c == TYMSAT_XOFF) {
                s->host_flow_held = 1;
                intercept = 1;
            } else if (c == TYMSAT_XON) {
                s->host_flow_held = 0;
                intercept = 1;
            }
        }

        if (intercept) {
            if (i > run_start && s->emit_remote != NULL) {
                s->emit_remote(s->ctx, data + run_start, i - run_start);
            }
            run_start = i + 1;
        }
    }

    if (len > run_start && s->emit_remote != NULL) {
        s->emit_remote(s->ctx, data + run_start, len - run_start);
    }
}

/* --- public entry points ---------------------------------------------- */

int tymsat_init(tymsat_session_t *s,
                const tymsat_config_t *cfg,
                tymsat_emit_fn emit_dte,
                tymsat_emit_fn emit_remote,
                void *ctx)
{
    if (s == NULL || cfg == NULL) return -1;

    memset(s, 0, sizeof(*s));
    s->cfg         = cfg;
    s->emit_dte    = emit_dte;
    s->emit_remote = emit_remote;
    s->ctx         = ctx;
    s->tid         = NULL;
    s->user        = NULL;
    s->state       = TYMSAT_STATE_IDLE;

    /* [HTU82:26-30]: "When you have connected to the network, TYMNET
       will display a request for your terminal identifier." */
    s->state = TYMSAT_STATE_AWAITING_TID;
    tymsat_emit_message(s, TYMSAT_MSG_TERMINAL_IDENTIFIER_REQ);
    return 0;
}

void tymsat_input_dte(tymsat_session_t *s, const uint8 *data, uint32 len)
{
    uint32 i;

    if (s == NULL || data == NULL) return;

    /* Data transfer is handled in bulk so that pass-through does not
       degrade to one callback per byte. */
    if (s->state == TYMSAT_STATE_DATA_TRANSFER) {
        feed_data_transfer(s, data, len);
        return;
    }

    for (i = 0; i < len; i++) {
        switch (s->state) {
        case TYMSAT_STATE_AWAITING_TID:
            feed_awaiting_tid(s, data[i]);
            break;
        case TYMSAT_STATE_AWAITING_LOGIN:
            feed_awaiting_login(s, data[i]);
            break;
        case TYMSAT_STATE_AWAITING_PASSWORD:
            feed_awaiting_password(s, data[i]);
            break;
        case TYMSAT_STATE_CIRCUIT_BUILD:
            /* Type-ahead during circuit build is preserved and
               replayed once the host answers. */
            if (s->pending_len < TYMSAT_PENDING_SIZE) {
                s->pending[s->pending_len++] = data[i];
            }
            break;
        case TYMSAT_STATE_DATA_TRANSFER:
            /* Reached only if a nested call changed state mid-loop. */
            feed_data_transfer(s, data + i, len - i);
            return;
        case TYMSAT_STATE_IDLE:
        case TYMSAT_STATE_CLEARED:
        default:
            break;
        }
    }
}

void tymsat_input_remote(tymsat_session_t *s, const uint8 *data, uint32 len)
{
    if (s == NULL || data == NULL || len == 0) return;
    if (s->state != TYMSAT_STATE_DATA_TRANSFER) return;

    /* host_flow_held records that the terminal has XOFF'd the host
       ([HTU82:113]). We do not buffer here: the TCP transport supplies
       the backpressure a real network provided with its own buffers.
       Recorded as a deviation. */
    if (s->emit_dte != NULL) {
        s->emit_dte(s->ctx, data, len);
    }
}

int tymsat_tick(tymsat_session_t *s, uint32 elapsed_20ths)
{
    if (s == NULL) return 0;

    /* [HTU82:262] starts the two-minute window at the terminal
       identifier entry, so it covers the login and password states
       only -- not the wait for the identifier itself, and not an
       established session. */
    if (s->state != TYMSAT_STATE_AWAITING_LOGIN &&
        s->state != TYMSAT_STATE_AWAITING_PASSWORD) {
        return 0;
    }

    s->login_ticks += elapsed_20ths;
    if (s->login_ticks < TYMSAT_LOGIN_TIMEOUT_20THS) return 0;

    tymsat_emit_message(s, TYMSAT_MSG_PLS_SEE_YOUR_REP);
    return 1;
}

void tymsat_circuit_connected(tymsat_session_t *s)
{
    if (s == NULL || s->state != TYMSAT_STATE_CIRCUIT_BUILD) return;
    enter_data_transfer(s);
}

void tymsat_circuit_failed(tymsat_session_t *s, tymsat_msg_t reason)
{
    if (s == NULL || s->state != TYMSAT_STATE_CIRCUIT_BUILD) return;
    tymsat_emit_message(s, reason);
    return_to_login(s);
}

void tymsat_circuit_cleared(tymsat_session_t *s, tymsat_msg_t reason)
{
    if (s == NULL) return;
    if (s->state != TYMSAT_STATE_DATA_TRANSFER &&
        s->state != TYMSAT_STATE_CIRCUIT_BUILD) {
        return;
    }

    s->state = TYMSAT_STATE_CLEARED;
    tymsat_emit_message(s, reason);
    return_to_login(s);
}
