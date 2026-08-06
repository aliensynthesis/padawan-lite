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

/* Tests for the stand-alone TYMSAT front end.

   Reference input/output pairs are taken from the two primary sources
   cited throughout src/tymsat.c and include/tymsat.h:

     [HTU82]  kb/How_to_Use_TYMNET_1982_OCR.txt, cited by line number
     [NPCF85] Network Products Concepts and Facilities (Jan/Jul 1985),
              cited by the document's own page numbers

   Each test names the behaviour and the source location that fixes
   it, in the manner the ITU-clause tests use for X.3/X.28/X.29. */

#include "test.h"
#include "tymsat.h"
#include "x25_stub.h"
#include <string.h>

/* --- capture harness -------------------------------------------------- */

static uint8  g_dte[4096];
static uint32 g_dte_len;
static uint8  g_remote[4096];
static uint32 g_remote_len;

static void cb_dte(void *ctx, const uint8 *data, uint32 len)
{
    (void)ctx;
    if (g_dte_len + len <= sizeof(g_dte)) {
        memcpy(g_dte + g_dte_len, data, len);
        g_dte_len += len;
    }
}

static void cb_remote(void *ctx, const uint8 *data, uint32 len)
{
    (void)ctx;
    if (g_remote_len + len <= sizeof(g_remote)) {
        memcpy(g_remote + g_remote_len, data, len);
        g_remote_len += len;
    }
}

static void reset_io(void)
{
    g_dte_len    = 0;
    g_remote_len = 0;
}

static int buf_contains(const uint8 *hay, uint32 hay_len, const char *needle)
{
    uint32 nl = (uint32)strlen(needle);
    uint32 i;
    if (nl == 0 || nl > hay_len) return 0;
    for (i = 0; i + nl <= hay_len; i++) {
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    }
    return 0;
}

static int dte_has(const char *needle)
{
    return buf_contains(g_dte, g_dte_len, needle);
}

static int dte_count(const char *needle)
{
    uint32 nl = (uint32)strlen(needle);
    uint32 i;
    int    n = 0;
    if (nl == 0 || nl > g_dte_len) return 0;
    for (i = 0; i + nl <= g_dte_len; i++) {
        if (memcmp(g_dte + i, needle, nl) == 0) n++;
    }
    return n;
}

static int remote_has(const char *needle)
{
    return buf_contains(g_remote, g_remote_len, needle);
}

static void feed(tymsat_session_t *s, const char *text)
{
    tymsat_input_dte(s, (const uint8 *)text, (uint32)strlen(text));
}

/* --- fixtures --------------------------------------------------------- */

/* username, password, default_host, ignore_host */
static const tymsat_user_t USERS[] = {
    { "DAVID",       "secret", "3020", 0 },  /* ordinary: password + home */
    { "INFORMATION", "",       "3020", 1 },  /* No Password + Ignore Host */
    { "MULTI",       "pw",     "",     0 }   /* no home: must name a host */
};

#define USER_COUNT ((uint16)(sizeof(USERS) / sizeof(USERS[0])))

static tymsat_config_t make_cfg(void)
{
    tymsat_config_t c;
    memset(&c, 0, sizeof(c));
    c.node_number        = 4242;
    c.port_number        = 56;
    c.emit_slot_number   = 0;
    c.slot_number        = 0;
    c.uppercase_messages = 0;
    c.accept_msg         = TYMSAT_ACCEPT_CALL_CONNECTED;
    c.users              = USERS;
    c.user_count         = USER_COUNT;
    return c;
}

/* Drive a session to the login prompt with terminal identifier 'A'. */
static void start_session(tymsat_session_t *s, const tymsat_config_t *cfg)
{
    reset_io();
    x25_stub_set_async(0);
    tymsat_init(s, cfg, cb_dte, cb_remote, NULL);
    feed(s, "A");
}

/* --- terminal identifier table ---------------------------------------- */

/* [HTU82:127-137] lists exactly A,B,C,D,E,F,G,I,P. There is no H and no
   letters beyond I apart from P. */
static void test_tid_table_matches_pamphlet(void)
{
    uint16              n = 0;
    const tymsat_tid_t *t = tymsat_tid_table(&n);

    ASSERT_TRUE(t != NULL);
    ASSERT_EQ_INT(n, 9);

    ASSERT_TRUE(tymsat_tid_by_char('A') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('B') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('C') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('D') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('E') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('F') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('G') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('I') != NULL);
    ASSERT_TRUE(tymsat_tid_by_char('P') != NULL);

    /* Absent from the table: H is not an identifier (it is the
       half-duplex control character, [HTU82:110]), and neither are
       J or Z. */
    ASSERT_TRUE(tymsat_tid_by_char('H') == NULL);
    ASSERT_TRUE(tymsat_tid_by_char('J') == NULL);
    ASSERT_TRUE(tymsat_tid_by_char('Z') == NULL);
}

/* [HTU82:129] A = 30cps and 120cps, CRT terminals and personal
   computers. [HTU82:132] D = 10cps. [HTU82:136] I = 120cps. */
static void test_tid_speeds_from_pamphlet(void)
{
    ASSERT_EQ_INT(tymsat_tid_by_char('A')->cps, 30);
    ASSERT_EQ_INT(tymsat_tid_by_char('A')->cps_alt, 120);
    ASSERT_EQ_INT(tymsat_tid_by_char('B')->cps, 15);
    ASSERT_EQ_INT(tymsat_tid_by_char('D')->cps, 10);
    ASSERT_EQ_INT(tymsat_tid_by_char('I')->cps, 120);
    ASSERT_EQ_INT(tymsat_tid_by_char('I')->cps_alt, 0);
}

/* [HTU82:137] "P + carriage return | EBCD / Correspondence" -- the only
   identifier that is non-ASCII and the only one taking a terminator. */
static void test_tid_p_is_ebcd_and_needs_cr(void)
{
    const tymsat_tid_t *p = tymsat_tid_by_char('P');
    ASSERT_EQ_INT(p->ebcd, 1);
    ASSERT_EQ_INT(p->needs_cr, 1);
    /* Every other identifier is ASCII and unterminated. */
    ASSERT_EQ_INT(tymsat_tid_by_char('A')->ebcd, 0);
    ASSERT_EQ_INT(tymsat_tid_by_char('A')->needs_cr, 0);
}

/* [HTU82:121-122] E and I are the identifiers documented as providing
   carriage-return delay. */
static void test_tid_cr_delay_flags(void)
{
    ASSERT_EQ_INT(tymsat_tid_by_char('E')->cr_delay, 1);
    ASSERT_EQ_INT(tymsat_tid_by_char('I')->cr_delay, 1);
    ASSERT_EQ_INT(tymsat_tid_by_char('A')->cr_delay, 0);
}

static void test_tid_lookup_is_case_insensitive(void)
{
    ASSERT_TRUE(tymsat_tid_by_char('a') == tymsat_tid_by_char('A'));
    ASSERT_TRUE(tymsat_tid_by_char('p') == tymsat_tid_by_char('P'));
}

/* --- handshake -------------------------------------------------------- */

/* [HTU82:26-30] "TYMNET will display a request for your terminal
   identifier. / please type your terminal identifier" */
static void test_init_requests_terminal_identifier(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    ASSERT_EQ_INT(tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL), 0);
    ASSERT_TRUE(dte_has("please type your terminal identifier"));
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_TID);
}

/* [HTU82:34-37] the node/port line then the login prompt:
       -NNNN-PPP-
       please log in:                                                  */
static void test_tid_yields_node_port_and_login_prompt(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    reset_io();

    feed(&s, "A");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
    ASSERT_TRUE(dte_has("-4242-56-"));
    ASSERT_TRUE(dte_has("please log in:"));
    ASSERT_TRUE(s.tid != NULL);
    ASSERT_EQ_INT(s.tid->id, 'A');
}

/* [HTU82:84] "TYMNET displays three numbers in certain connections,
   such as those made via WATS lines... node... slot... port." */
static void test_three_number_wats_form(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.emit_slot_number = 1;
    cfg.slot_number      = 3;

    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    reset_io();
    feed(&s, "A");
    ASSERT_TRUE(dte_has("-4242-3-56-"));
}

/* [HTU82:137] the P identifier takes a following carriage return; the
   login prompt must not appear until it arrives. */
static void test_p_identifier_waits_for_cr(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    reset_io();

    feed(&s, "P");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_TID);
    ASSERT_TRUE(!dte_has("please log in:"));

    feed(&s, "\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
    ASSERT_TRUE(dte_has("please log in:"));
}

/* [HTU82:32] the identifier request "may appear garbled... It may not
   appear at some terminals, at all. Wait a few seconds and type your
   identifier" -- i.e. the TYMSAT tolerates noise rather than erroring.
   Unrecognised characters are ignored and we keep waiting. */
static void test_unknown_identifier_is_ignored(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    reset_io();

    feed(&s, "%\r\n9");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_TID);
    ASSERT_TRUE(!dte_has("please log in:"));

    feed(&s, "A");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
}

/* --- login ------------------------------------------------------------ */

/* [HTU82:39-47] username then CR, then the password prompt. */
static void test_username_then_password_prompt(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();

    feed(&s, "DAVID\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_PASSWORD);
    ASSERT_TRUE(dte_has("password:"));
    ASSERT_EQ_INT(strcmp(s.username, "DAVID"), 0);
}

/* The TYMSAT echoes input unless half duplex was selected; [HTU82:110]
   defines ^H as the thing that suppresses "TYMSAT echoing of input
   characters", which implies echo is otherwise on. */
static void test_username_is_echoed(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();
    feed(&s, "DAVID");
    ASSERT_TRUE(dte_has("DAVID"));
}

/* [HTU82:47] "Passwords are not displayed at full-duplex terminals for
   security reasons." */
static void test_password_is_not_echoed(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();

    feed(&s, "secret");
    ASSERT_TRUE(!dte_has("secret"));
    ASSERT_EQ_INT(strcmp(s.password, "secret"), 0);
}

/* Default connect acknowledgement: "call connected", the observed
   behaviour of a 1986 TYMNET client. A zero-initialised config must
   land here rather than on either pamphlet example. */
static void test_successful_login_emits_call_connected(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    ASSERT_EQ_INT(cfg.accept_msg, TYMSAT_ACCEPT_CALL_CONNECTED);
    ASSERT_EQ_INT(TYMSAT_ACCEPT_CALL_CONNECTED, 0);   /* memset default */

    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(dte_has("call connected"));
    ASSERT_TRUE(!dte_has("host is online"));
    ASSERT_TRUE(!dte_has(";"));
}

/* [HTU82:49-53] "an acceptance message, such as a semicolon (;)". */
static void test_successful_login_emits_terse_acceptance(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.accept_msg = TYMSAT_ACCEPT_TERSE;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(dte_has(";"));
    ASSERT_TRUE(!dte_has("host is online"));
    ASSERT_TRUE(!dte_has("call connected"));
}

/* [HTU82:49-53] and the catalogue entry at [HTU82:231]. */
static void test_successful_login_emits_verbose_acceptance(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.accept_msg = TYMSAT_ACCEPT_VERBOSE;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(dte_has("host is online"));
    ASSERT_TRUE(!dte_has("call connected"));
}

/* The acknowledgement honours the message-case setting, since unlike
   the bare semicolon it is a word message. */
static void test_call_connected_honours_uppercase(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.uppercase_messages = 1;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    ASSERT_TRUE(dte_has("CALL CONNECTED"));
    ASSERT_TRUE(!dte_has("call connected"));
}

/* [NPCF85 p. 14-6] No Password: "the user logs in to the network
   without using a password" -- the prompt is skipped entirely. */
static void test_no_password_user_skips_password_prompt(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();
    feed(&s, "INFORMATION\r");

    ASSERT_TRUE(!dte_has("password:"));
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
}

/* [NPCF85 p. 14-6] the colon separates username from destination host
   number: "entering a colon (:) and a host number in the login
   string". */
static void test_colon_selects_destination_host(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "MULTI:1776\r");
    ASSERT_EQ_INT(strcmp(s.username, "MULTI"), 0);
    ASSERT_EQ_INT(strcmp(s.host_number, "1776"), 0);
}

/* A user with no configured home must name a destination; omitting it
   is [HTU82:246] NO HOST SPECIFIED. */
static void test_missing_destination_yields_no_host_specified(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "MULTI\r");
    reset_io();
    feed(&s, "pw\r");

    ASSERT_TRUE(dte_has("no host specified"));
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
    ASSERT_TRUE(dte_has("please log in:"));
}

/* [NPCF85 p. 14-6] Ignore Host: the destination comes from the user's
   profile, not from what was typed. */
static void test_ignore_host_discards_typed_destination(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "INFORMATION:9999\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    /* The configured home (3020) won over the typed 9999. */
    ASSERT_EQ_INT(strcmp(s.host_number, "3020"), 0);
}

/* --- login errors ----------------------------------------------------- */

/* [HTU82:288] USER NAME: "A carriage return or line feed has been
   entered instead of a user name. Type your user name on the same line
   as the prompt." The message doubles as the re-prompt, so exactly one
   prompt appears -- not the message plus a fresh "please log in:". */
static void test_bare_cr_at_login_yields_user_name_message(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();
    feed(&s, "\r");

    ASSERT_TRUE(dte_has("user name"));
    ASSERT_EQ_INT(dte_count("user name"), 1);
    ASSERT_EQ_INT(dte_count("please log in:"), 0);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
}

/* [HTU82:255] PASSWORD: "A carriage return or line feed has been
   entered instead of a password. Type your password on the same line
   as the prompt." Message and prompt are the same string here, so an
   empty entry must reprint it exactly once. */
static void test_bare_cr_at_password_yields_password_message(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "\r");

    ASSERT_TRUE(dte_has("password:"));
    ASSERT_EQ_INT(dte_count("password:"), 1);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_PASSWORD);
}

/* [HTU82:222] ERROR, TYPE USER NAME: "An attempt has been made to log
   in under an invalid user name. Type your correct user name." */
static void test_unknown_username_reprompts(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();
    feed(&s, "NOBODY\r");

    ASSERT_TRUE(dte_has("error, type user name"));
    ASSERT_TRUE(dte_has("please log in:"));
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
}

/* [HTU82:225] ERROR, TYPE PASSWORD: "An invalid password has been
   entered. Type your correct password." -- so we re-prompt for the
   password, not for the whole login. */
static void test_wrong_password_reprompts_for_password(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "wrong\r");

    ASSERT_TRUE(dte_has("error, type password"));
    /* Unlike the empty-entry case, "error, type password" is not
       itself a prompt, so the prompt follows it. */
    ASSERT_EQ_INT(dte_count("password:"), 1);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_PASSWORD);
    /* The failed attempt is cleared, so a correct retry succeeds. */
    reset_io();
    feed(&s, "secret\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
}

/* --- control characters ----------------------------------------------- */

/* [HTU82:108-115] and [NPCF85 p. 5-8]: control characters are embedded
   in the login string and select session options. They must not become
   part of the username. [HTU82:141] "enter Control R and Control X
   immediately before your user name." */
static void test_control_characters_are_stripped_and_recorded(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "\022\030DAVID\r");   /* ^R ^X DAVID CR */

    ASSERT_EQ_INT(strcmp(s.username, "DAVID"), 0);
    ASSERT_TRUE((s.ctl_flags & TYMSAT_CTL_TERM_FLOW) != 0);
    ASSERT_TRUE((s.ctl_flags & TYMSAT_CTL_NET_FLOW) != 0);
    ASSERT_TRUE((s.ctl_flags & TYMSAT_CTL_EVEN_PARITY) == 0);
}

/* [HTU82:110] "Control H - Initiates half-duplex operation,
   suppressing TYMSAT echoing of input characters." */
static void test_half_duplex_suppresses_echo(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();
    feed(&s, "\010DAVID");        /* ^H DAVID */

    ASSERT_TRUE((s.ctl_flags & TYMSAT_CTL_HALF_DUPLEX) != 0);
    ASSERT_TRUE(!dte_has("DAVID"));
}

/* [HTU82:111] "Control P - Provides even parity for computer output." */
static void test_even_parity_flag(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "\020DAVID\r");      /* ^P DAVID CR */
    ASSERT_TRUE((s.ctl_flags & TYMSAT_CTL_EVEN_PARITY) != 0);
    ASSERT_EQ_INT(strcmp(s.username, "DAVID"), 0);
}

/* --- data transfer ---------------------------------------------------- */

static void test_data_passes_through_once_connected(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "INFORMATION\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);

    reset_io();
    feed(&s, "hello");
    ASSERT_TRUE(remote_has("hello"));

    tymsat_input_remote(&s, (const uint8 *)"world", 5);
    ASSERT_TRUE(dte_has("world"));
}

/* [HTU82:113-115] "Control S is effective only when a Control R has
   been entered at log in." Without ^R the bytes are ordinary data. */
static void test_xoff_is_data_without_control_r(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "INFORMATION\r");
    reset_io();

    feed(&s, "a\023b");           /* a ^S b */
    ASSERT_EQ_INT(g_remote_len, 3);
    ASSERT_EQ_INT(s.host_flow_held, 0);
}

/* With ^R entered at login, ^S/^Q become flow control and are consumed
   rather than forwarded to the host. */
static void test_xoff_intercepted_with_control_r(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "\022INFORMATION\r");    /* ^R INFORMATION CR */
    reset_io();

    feed(&s, "a\023b");               /* a ^S b */
    ASSERT_EQ_INT(g_remote_len, 2);   /* the ^S was consumed */
    ASSERT_TRUE(remote_has("ab"));
    ASSERT_EQ_INT(s.host_flow_held, 1);

    feed(&s, "\021");                 /* ^Q resumes */
    ASSERT_EQ_INT(s.host_flow_held, 0);
}

/* Type-ahead during circuit build is preserved and replayed once the
   host answers. */
static void test_typeahead_during_circuit_build_is_replayed(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    x25_stub_set_async(1);
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    feed(&s, "A");
    feed(&s, "INFORMATION\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_CIRCUIT_BUILD);

    reset_io();
    feed(&s, "typed early");
    ASSERT_EQ_INT(g_remote_len, 0);   /* held, not forwarded yet */

    tymsat_circuit_connected(&s);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(remote_has("typed early"));

    x25_stub_set_async(0);
}

/* --- teardown --------------------------------------------------------- */

/* [HTU82:57-61] "After you have logged off from the host computer, you
   will receive the message: please log in: / You may log in to the same
   host or to another, or you may hang up." -- carrier stays up and the
   terminal identifier is NOT requested again. */
static void test_logoff_returns_to_login_without_new_tid(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "INFORMATION\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);

    reset_io();
    tymsat_circuit_cleared(&s, TYMSAT_MSG_DROPPED_BY_HOST_SYSTEM);

    ASSERT_TRUE(dte_has("dropped by host system"));
    ASSERT_TRUE(dte_has("please log in:"));
    ASSERT_TRUE(!dte_has("please type your terminal identifier"));
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
    /* The identifier survives the logoff. */
    ASSERT_TRUE(s.tid != NULL);
    ASSERT_EQ_INT(s.tid->id, 'A');

    /* A second login on the same carrier works. */
    reset_io();
    feed(&s, "INFORMATION\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
}

/* --- login timer ------------------------------------------------------ */

/* [HTU82:262] PLS SEE YOUR REP: "A valid user name or password has not
   been entered within two minutes following the terminal identifier
   entry." 120 s = 2400 twentieths. */
static void test_two_minute_login_timeout(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    reset_io();

    /* One tick short of the limit: nothing happens. */
    ASSERT_EQ_INT(tymsat_tick(&s, TYMSAT_LOGIN_TIMEOUT_20THS - 1), 0);
    ASSERT_TRUE(!dte_has("pls see your rep"));

    /* Crossing the limit fires once and tells the caller to hang up. */
    ASSERT_EQ_INT(tymsat_tick(&s, 1), 1);
    ASSERT_TRUE(dte_has("pls see your rep"));
}

/* The two-minute window is a login limit; an established session is not
   subject to it. */
static void test_timer_does_not_fire_during_session(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    feed(&s, "INFORMATION\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);

    reset_io();
    ASSERT_EQ_INT(tymsat_tick(&s, TYMSAT_LOGIN_TIMEOUT_20THS * 4), 0);
    ASSERT_TRUE(!dte_has("pls see your rep"));
}

/* Entering a field resets the window, per "has not been entered
   within two minutes" -- progress restarts the clock. */
static void test_timer_resets_on_field_entry(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    start_session(&s, &cfg);
    tymsat_tick(&s, TYMSAT_LOGIN_TIMEOUT_20THS - 1);
    feed(&s, "DAVID\r");          /* progress: username accepted */
    reset_io();

    ASSERT_EQ_INT(tymsat_tick(&s, 2), 0);
    ASSERT_TRUE(!dte_has("pls see your rep"));
}

/* An event-loop driver must keep ticking while the login limit runs,
   or the limit never expires. tymsat_has_pending_timer is what tells
   it so; these assertions pin the states it must cover to the states
   tymsat_tick actually acts on. */
static void test_pending_timer_tracks_tick_states(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
    /* Awaiting the terminal identifier: the limit starts AFTER the
       identifier is entered, so nothing is counting down yet. */
    ASSERT_EQ_INT(tymsat_has_pending_timer(&s), 0);

    feed(&s, "A");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_LOGIN);
    ASSERT_EQ_INT(tymsat_has_pending_timer(&s), 1);

    feed(&s, "DAVID\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_AWAITING_PASSWORD);
    ASSERT_EQ_INT(tymsat_has_pending_timer(&s), 1);

    feed(&s, "secret\r");
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_EQ_INT(tymsat_has_pending_timer(&s), 0);

    ASSERT_EQ_INT(tymsat_has_pending_timer(NULL), 0);
}

/* Every state for which tymsat_tick can do something must be reported
   by tymsat_has_pending_timer, or a driver will sleep through it. */
static void test_pending_timer_agrees_with_tick(void)
{
    static const tymsat_state_t STATES[] = {
        TYMSAT_STATE_IDLE, TYMSAT_STATE_AWAITING_TID,
        TYMSAT_STATE_AWAITING_LOGIN, TYMSAT_STATE_AWAITING_PASSWORD,
        TYMSAT_STATE_CIRCUIT_BUILD, TYMSAT_STATE_DATA_TRANSFER,
        TYMSAT_STATE_CLEARED
    };
    tymsat_config_t cfg = make_cfg();
    int i;

    for (i = 0; i < (int)(sizeof(STATES) / sizeof(STATES[0])); i++) {
        tymsat_session_t s;
        int advertised;
        int acted;

        reset_io();
        tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);
        s.state = STATES[i];
        s.login_ticks = 0;

        advertised = tymsat_has_pending_timer(&s);
        /* A tick large enough to expire anything: if tick changes the
           session or emits, the state was time-driven. */
        reset_io();
        (void)tymsat_tick(&s, TYMSAT_LOGIN_TIMEOUT_20THS);
        acted = (s.login_ticks != 0) || (g_dte_len != 0);

        ASSERT_EQ_INT(advertised, acted);
    }
}

/* --- network-path emulation ------------------------------------------- */

/* The delay is applied AFTER credentials are accepted and BEFORE the
   call is placed, standing in for the routing and needle-threading a
   real TYMNET did before the destination node reached the host
   ([NPCF85 p. 4-14]). While it runs, no call exists. */
static void test_path_delay_defers_the_call(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.path_delay_20ths = 20;            /* 1000 ms */
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    /* Credentials accepted, but we are waiting on the network. */
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_PATH_DELAY);
    /* The x25 stub stamps call_id on call; an unplaced call leaves it
       at the value tymsat_init zeroed. */
    ASSERT_EQ_INT(s.call.call_id, 0);
    ASSERT_TRUE(!dte_has("call connected"));
    /* Destination was resolved before the wait began. */
    ASSERT_EQ_INT(strcmp(s.host_number, "3020"), 0);
}

static void test_path_delay_expires_then_connects(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.path_delay_20ths = 20;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    feed(&s, "secret\r");
    reset_io();

    /* One tick short: still waiting, still no call. */
    ASSERT_EQ_INT(tymsat_tick(&s, 19), 0);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_PATH_DELAY);
    ASSERT_EQ_INT(s.call.call_id, 0);
    ASSERT_TRUE(!dte_has("call connected"));

    /* Crossing it places the call and acknowledges. */
    ASSERT_EQ_INT(tymsat_tick(&s, 1), 0);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(dte_has("call connected"));
}

/* A driver must keep ticking through the delay or it never expires. */
static void test_path_delay_reports_pending_timer(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.path_delay_20ths = 20;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    feed(&s, "secret\r");

    ASSERT_EQ_INT(s.state, TYMSAT_STATE_PATH_DELAY);
    ASSERT_EQ_INT(tymsat_has_pending_timer(&s), 1);
}

/* Type-ahead during the wait survives, exactly as during circuit build. */
static void test_path_delay_preserves_typeahead(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.path_delay_20ths = 20;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    feed(&s, "secret\r");
    reset_io();

    feed(&s, "typed during the wait");
    ASSERT_EQ_INT(g_remote_len, 0);       /* held, no circuit yet */

    (void)tymsat_tick(&s, 20);
    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(remote_has("typed during the wait"));
}

/* Zero disables it: the call is placed inline, as before the feature. */
static void test_path_delay_zero_connects_immediately(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.path_delay_20ths = 0;
    start_session(&s, &cfg);
    feed(&s, "DAVID\r");
    reset_io();
    feed(&s, "secret\r");

    ASSERT_EQ_INT(s.state, TYMSAT_STATE_DATA_TRANSFER);
    ASSERT_TRUE(dte_has("call connected"));
}

/* The documented default is 1000 ms; the driver converts at 20 Hz. */
static void test_path_delay_default_constants(void)
{
    ASSERT_EQ_INT(TYMSAT_DEFAULT_PATH_DELAY_MS, 1000);
    ASSERT_EQ_INT(TYMSAT_DEFAULT_PATH_DELAY_20THS, 20);
    /* 1000 ms at 50 ms per tick. */
    ASSERT_EQ_INT((TYMSAT_DEFAULT_PATH_DELAY_MS + 49) / 50,
                  TYMSAT_DEFAULT_PATH_DELAY_20THS);
}

/* --- message catalogue ------------------------------------------------ */

/* Spot-check the catalogue against [HTU82:196-289]. */
static void test_message_text_matches_pamphlet(void)
{
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_ACCESS_NOT_PERMITTED),
                         "access not permitted"), 0);
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_BAD_HOST_NUMBER),
                         "bad host number"), 0);
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_HOST_IS_ONLINE),
                         "host is online"), 0);
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_PLEASE_LOG_IN),
                         "please log in:"), 0);
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_PLS_SEE_YOUR_REP),
                         "pls see your rep"), 0);
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_LOGON_ABORTED),
                         "logon aborted... disconnecting"), 0);
    /* Client-observed, not from the pamphlet catalogue. */
    ASSERT_EQ_INT(strcmp(tymsat_message_text(TYMSAT_MSG_CALL_CONNECTED),
                         "call connected"), 0);
}

static void test_message_text_bounds(void)
{
    ASSERT_TRUE(tymsat_message_text(TYMSAT_MSG_COUNT) == NULL);
    ASSERT_TRUE(tymsat_message_text((tymsat_msg_t)-1) == NULL);
    /* Every catalogue slot is populated. */
    {
        int i;
        for (i = 0; i < (int)TYMSAT_MSG_COUNT; i++) {
            ASSERT_TRUE(tymsat_message_text((tymsat_msg_t)i) != NULL);
        }
    }
}

/* The uppercase_messages setting flips the whole catalogue; see the
   sourcing conflict documented in include/tymsat.h. */
static void test_uppercase_message_mode(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    cfg.uppercase_messages = 1;
    reset_io();
    tymsat_init(&s, &cfg, cb_dte, cb_remote, NULL);

    ASSERT_TRUE(dte_has("PLEASE TYPE YOUR TERMINAL IDENTIFIER"));
    ASSERT_TRUE(!dte_has("please type your terminal identifier"));

    reset_io();
    feed(&s, "A");
    ASSERT_TRUE(dte_has("PLEASE LOG IN:"));
}

/* --- argument handling ------------------------------------------------ */

static void test_init_rejects_null_arguments(void)
{
    tymsat_session_t s;
    tymsat_config_t  cfg = make_cfg();

    ASSERT_TRUE(tymsat_init(NULL, &cfg, cb_dte, cb_remote, NULL) != 0);
    ASSERT_TRUE(tymsat_init(&s, NULL, cb_dte, cb_remote, NULL) != 0);
}

int main(void)
{
    test_tid_table_matches_pamphlet();
    test_tid_speeds_from_pamphlet();
    test_tid_p_is_ebcd_and_needs_cr();
    test_tid_cr_delay_flags();
    test_tid_lookup_is_case_insensitive();

    test_init_requests_terminal_identifier();
    test_tid_yields_node_port_and_login_prompt();
    test_three_number_wats_form();
    test_p_identifier_waits_for_cr();
    test_unknown_identifier_is_ignored();

    test_username_then_password_prompt();
    test_username_is_echoed();
    test_password_is_not_echoed();
    test_successful_login_emits_call_connected();
    test_successful_login_emits_terse_acceptance();
    test_call_connected_honours_uppercase();
    test_successful_login_emits_verbose_acceptance();
    test_no_password_user_skips_password_prompt();
    test_colon_selects_destination_host();
    test_missing_destination_yields_no_host_specified();
    test_ignore_host_discards_typed_destination();

    test_bare_cr_at_login_yields_user_name_message();
    test_bare_cr_at_password_yields_password_message();
    test_unknown_username_reprompts();
    test_wrong_password_reprompts_for_password();

    test_control_characters_are_stripped_and_recorded();
    test_half_duplex_suppresses_echo();
    test_even_parity_flag();

    test_data_passes_through_once_connected();
    test_xoff_is_data_without_control_r();
    test_xoff_intercepted_with_control_r();
    test_typeahead_during_circuit_build_is_replayed();

    test_logoff_returns_to_login_without_new_tid();

    test_two_minute_login_timeout();
    test_timer_does_not_fire_during_session();
    test_timer_resets_on_field_entry();

    test_path_delay_defers_the_call();
    test_path_delay_expires_then_connects();
    test_path_delay_reports_pending_timer();
    test_path_delay_preserves_typeahead();
    test_path_delay_zero_connects_immediately();
    test_path_delay_default_constants();
    test_pending_timer_tracks_tick_states();
    test_pending_timer_agrees_with_tick();
    test_message_text_matches_pamphlet();
    test_message_text_bounds();
    test_uppercase_message_mode();

    test_init_rejects_null_arguments();

    TEST_REPORT();
}
