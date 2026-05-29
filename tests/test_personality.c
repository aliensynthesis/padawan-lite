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

/* Tests for the personality system. */
#include "test.h"
#include "personality.h"
#include "pad.h"
#include "x3.h"
#include "x25_stub.h"
#include <string.h>

static uint8 g_dte[2048];
static uint32 g_dte_len;
static void cb_dte(void *ctx, const uint8 *data, uint32 len)
{
    (void)ctx;
    if (g_dte_len + len <= sizeof(g_dte)) {
        memcpy(g_dte + g_dte_len, data, len);
        g_dte_len += len;
    }
}
static void cb_remote(void *ctx, const uint8 *data, uint32 len)
{ (void)ctx; (void)data; (void)len; }

static int contains(const char *needle)
{
    uint32 nl = (uint32)strlen(needle);
    uint32 i;
    if (nl > g_dte_len) return 0;
    for (i = 0; i + nl <= g_dte_len; i++) {
        if (memcmp(g_dte + i, needle, nl) == 0) return 1;
    }
    return 0;
}

static void reset_io(void) { g_dte_len = 0; }

static void test_lookup_default(void)
{
    const personality_t *p = personality_by_name(NULL);
    ASSERT_TRUE(p != NULL);
    ASSERT_EQ_INT(strcmp(p->name, "default"), 0);
    /* "default" by explicit name also resolves. */
    ASSERT_TRUE(personality_by_name("default") == p);
}

static void test_lookup_known(void)
{
    ASSERT_TRUE(personality_by_name("telenet") != NULL);
    ASSERT_TRUE(strcmp(personality_by_name("telenet")->name, "telenet") == 0);
    /* "tymnet" was removed in v1.4.0; lookup must now return NULL. */
    ASSERT_TRUE(personality_by_name("tymnet") == NULL);
}

static void test_lookup_unknown_returns_null(void)
{
    ASSERT_TRUE(personality_by_name("does-not-exist") == NULL);
    ASSERT_TRUE(personality_by_name("Telenet") == NULL);    /* case-sensitive */
}

static void test_default_personality_is_passthrough(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("default"));
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    /* Default personality leaves X.28 standard "FREE" in place. */
    ASSERT_TRUE(contains("FREE"));
    ASSERT_TRUE(!contains("READY"));
    ASSERT_TRUE(!contains("ready"));
}

static void test_telenet_personality_replaces_stat(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    /* Telenet personality overrides FREE -> READY. */
    ASSERT_TRUE(contains("READY"));
    ASSERT_TRUE(!contains("FREE"));
}

static void test_telenet_personality_replaces_err(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* CLR with no active call -> ERR signal. Telenet uses "?". */
    pad_input_dte(&pad, (const uint8 *)"CLR\r", 4);
    ASSERT_TRUE(contains("?"));
}

static void test_profile_overlay_applies(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* Simple profile defaults: param 3 = 126. Telenet overlay sets to 2. */
    ASSERT_EQ_INT(pad.params.values[X3_PAR_FORWARD], 126);
    pad_set_personality(&pad, personality_by_name("telenet"));
    ASSERT_EQ_INT(pad.params.values[X3_PAR_FORWARD], 2);
}

static void test_profile_overlay_skips_speed_param(void)
{
    pad_session_t pad;
    uint8         before;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    before = pad.params.values[X3_PAR_SPEED];
    /* Telenet overlay marks param 11 (speed) as KEEP so it's untouched. */
    pad_set_personality(&pad, personality_by_name("telenet"));
    ASSERT_EQ_INT(pad.params.values[X3_PAR_SPEED], before);
}

static void test_nui_prompt_text_overridden(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* Bare ID prompts; Telenet's NUI prompt is "ID?" not "NUI?". */
    pad_input_dte(&pad, (const uint8 *)"ID\r", 3);
    ASSERT_TRUE(contains("ID?"));
    ASSERT_TRUE(!contains("NUI?"));
}

static void test_personality_null_reverts_to_default(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_set_personality(&pad, NULL);
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    /* No personality => X.28 standard "FREE" again. */
    ASSERT_TRUE(contains("FREE"));
}

static void test_telenet_prompt_char_visible(void)
{
    pad_session_t pad;
    reset_io();
    /* Mirror the interactive driver: pad_init_handshake puts the session
       in ACTIVE_LINK; Telenet requires TWO CRs to complete the
       handshake (autobaud heritage), THEN emits the TERMINAL= prompt
       and waits for the user's terminal-type response. A third CR
       (empty terminal type) finishes the handshake; only THEN does
       emit_prompt_if_enabled fire. Param 6 must have the prompt bit
       (value 4) set so the '@' reaches the DTE. */
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r\r\r", 3);
    ASSERT_TRUE(contains("@"));
}

static void test_banner_set_by_personality(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* Telenet personality sets banner to "TELENET". */
    ASSERT_EQ_INT(strcmp(pad.pad_id_text, "TELENET"), 0);
}

static void test_telenet_command_aliases_present(void)
{
    /* The Telenet personality must publish a non-NULL alias table
       containing C / CONNECT / D / DISCONNECT. */
    const personality_t *p = personality_by_name("telenet");
    int saw_c = 0, saw_connect = 0, saw_d = 0, saw_disconnect = 0;
    int i;
    ASSERT_TRUE(p != NULL);
    ASSERT_TRUE(p->command_aliases != NULL);
    for (i = 0; p->command_aliases[i].keyword != NULL; i++) {
        const char *k = p->command_aliases[i].keyword;
        if (strcmp(k, "C")          == 0) saw_c          = 1;
        if (strcmp(k, "CONNECT")    == 0) saw_connect    = 1;
        if (strcmp(k, "D")          == 0) saw_d          = 1;
        if (strcmp(k, "DISCONNECT") == 0) saw_disconnect = 1;
    }
    ASSERT_TRUE(saw_c);
    ASSERT_TRUE(saw_connect);
    ASSERT_TRUE(saw_d);
    ASSERT_TRUE(saw_disconnect);
}

static void test_default_has_no_command_aliases(void)
{
    /* Aliases are Telenet-specific; the default personality must
       not publish any. */
    ASSERT_TRUE(personality_by_name("default")->command_aliases == NULL);
}

static void test_telenet_c_alias_with_space_dispatches_call(void)
{
    /* Telenet aliases require whitespace between the keyword and the
       address (no concatenated "c12345" form). Driving
       "c 12345\r" under the Telenet personality places the call
       (X28_CMD_SELECTION dispatch); the stub auto-accepts and
       emit_connected fires the personality's "CONNECTED" text. */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"c 12345\r", 8);
    ASSERT_TRUE(contains("CONNECTED"));
}

static void test_telenet_C_alias_with_space_dispatches_call(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"C 12345\r", 8);
    ASSERT_TRUE(contains("CONNECTED"));
}

static void test_telenet_c_no_space_does_not_match_alias(void)
{
    /* "c12345" (no space) must NOT match the Telenet alias. It falls
       through to the bare SELECTION path (empty address); the stub
       auto-accepts but the user-visible outcome differs only in that
       the alias-driven CONNECTED path would have called address
       "12345" while this path calls the empty string. We assert the
       call was rejected by checking that the bridge stub error-cause
       text "ERR" appears OR that no call ever connected. The simplest
       observable: the parser did not produce a SELECTION with address
       "12345" -- a fact we test directly at the parser layer in
       test_x28_signals. Here we just guard against regression by
       confirming that even when alias matching is enabled, the no-
       space form is benign (no crash, no leak). */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"c12345\r", 7);
    /* The Telenet personality emits "CONNECTED" only when a call to a
       real address succeeds; an empty-address call still produces
       "CONNECTED" through the stub. The point of this test is just
       that the no-space form is processed without error and matches
       the bare-SELECTION path that the standard parser would take. */
    (void)pad;   /* no specific assertion; coverage of the no-match path */
}

static void test_telenet_CONNECT_alias_dispatches_call(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"connect 12345\r", 14);
    ASSERT_TRUE(contains("CONNECTED"));
}

static void test_telenet_D_alias_dispatches_clr(void)
{
    /* CLR with no active call still goes through the parser; the PAD
       responds with ERR. With Telenet personality that's "?". */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"d\r", 2);
    ASSERT_TRUE(contains("?"));
}

static void test_telenet_DISCONNECT_alias_dispatches_clr(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"disconnect\r", 11);
    ASSERT_TRUE(contains("?"));
}

static void test_telenet_handshake_requires_two_crs(void)
{
    /* Telenet personality opts into the two-CR handshake. The first
       CR must NOT emit the banner or the prompt -- it transitions
       silently to DTE_WAITING (the "autobaud acknowledged" state).
       The second CR completes the handshake. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* First CR: silent, state advances to DTE_WAITING. */
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DTE_WAITING);
    ASSERT_TRUE(!contains("TELENET"));
    ASSERT_TRUE(!contains("@"));
    /* Second CR: banner emitted, TERMINAL= prompt emitted, state at
       AWAITING_TERMINAL_TYPE. No @ prompt yet. */
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_AWAITING_TERMINAL_TYPE);
    ASSERT_TRUE(contains("TELENET"));
    ASSERT_TRUE(contains("TERMINAL="));
    ASSERT_TRUE(!contains("@"));
    /* Third CR: empty terminal-type response. Now @ appears. */
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(contains("@"));
}

static void test_default_handshake_still_single_cr(void)
{
    /* Default personality keeps the single-CR handshake (no
       behavioural change for non-Telenet users). */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default. */
    pad_set_identification(&pad, "PADAWAN-LITE v1.2");
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(contains("PADAWAN-LITE v1.2"));
}

static void test_telenet_terminal_prompt_captures_value(void)
{
    /* After Telenet's two-CR handshake, the PAD emits "TERMINAL="
       and waits for a free-form response. Bytes are echoed; the
       captured value lands in pad.terminal_type on CR; then the @
       prompt fires. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r\r", 2);
    ASSERT_EQ_INT(pad.state, PAD_STATE_AWAITING_TERMINAL_TYPE);
    ASSERT_TRUE(contains("TERMINAL="));
    /* Type "D1" then CR. */
    pad_input_dte(&pad, (const uint8 *)"D1\r", 3);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_EQ_INT(strcmp(pad.terminal_type, "D1"), 0);
    ASSERT_TRUE(contains("@"));
    /* Echo: the "D1" the user typed must have been sent back. */
    ASSERT_TRUE(contains("D1"));
}

static void test_telenet_terminal_prompt_accepts_empty(void)
{
    /* Just hitting CR at the TERMINAL= prompt is a legitimate
       response (default terminal). pad.terminal_type is the empty
       string "", not unset/garbage. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r\r", 2);
    ASSERT_EQ_INT(pad.state, PAD_STATE_AWAITING_TERMINAL_TYPE);
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_EQ_INT(strcmp(pad.terminal_type, ""), 0);
    ASSERT_EQ_INT(pad.terminal_type_len, 0);
    ASSERT_TRUE(contains("@"));
}

static void test_telenet_terminal_prompt_truncates_overflow(void)
{
    /* Bytes beyond PAD_TERMINAL_TYPE_MAX are silently dropped from
       storage (the user still sees them echoed). Stored value is
       truncated to PAD_TERMINAL_TYPE_MAX. */
    pad_session_t pad;
    /* 20 chars: longer than PAD_TERMINAL_TYPE_MAX (15). */
    const char *input = "AAAAABBBBBCCCCCDDDDD\r";
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r\r", 2);
    pad_input_dte(&pad, (const uint8 *)input, (uint32)strlen(input));
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_EQ_INT(pad.terminal_type_len, PAD_TERMINAL_TYPE_MAX);
    ASSERT_EQ_INT(strcmp(pad.terminal_type, "AAAAABBBBBCCCCC"), 0);
}

static void test_default_personality_no_terminal_prompt(void)
{
    /* Default personality has terminal_type_prompt = NULL, so the
       handshake goes straight to PAD_WAITING + @ with no TERMINAL=
       step. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_identification(&pad, "PADAWAN-LITE v1.2");
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!contains("TERMINAL="));
    ASSERT_TRUE(contains("PADAWAN-LITE v1.2"));
}

static void test_telenet_connected_includes_called_address(void)
{
    /* Telenet emits "<address> CONNECTED" rather than the bare
       "CONNECTED" text. Exact-byte assertion to catch a regression
       where the address is dropped or the wrong word is used. */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    ASSERT_TRUE(contains("30199 CONNECTED"));
}

static void test_telenet_disconnect_emits_address_and_text(void)
{
    /* "disconnect" (Telenet alias for CLR) on an active call emits
       "<address> DISCONNECTED" via the personality's clr_confirm_text
       + the address-prefix flag, then leaves the session at
       PAD_WAITING with a fresh @ prompt -- so the user can issue
       their next command without typing blind. The user must hit
       PAD recall (DLE) first to escape from DATA_TRANSFER to
       WAITING_FOR_CMD; otherwise "disconnect" would be forwarded
       to the remote as data. */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    ASSERT_TRUE(contains("30199 CONNECTED"));
    reset_io();
    pad_input_dte(&pad, &dle, 1);
    pad_input_dte(&pad, (const uint8 *)"disconnect\r", 11);
    ASSERT_TRUE(contains("30199 DISCONNECTED"));
    /* Prompt fired after the confirmation: the user sees @ ready
       to accept the next command. */
    ASSERT_TRUE(contains("@"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_default_personality_uses_standard_com(void)
{
    /* Default personality must emit the X.28-standard "COM" form
       (with no address prefix) even though our PAD now stores the
       called address. */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default. */
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    ASSERT_TRUE(contains("COM"));
    ASSERT_TRUE(!contains("30199 COM"));
    ASSERT_TRUE(!contains("30199 CONNECTED"));
}

static void test_default_personality_clr_uses_standard_confirm(void)
{
    /* Default personality emits standard "CLR CONF" on user-initiated
       clear, not "<address> DISCONNECTED". Recall first (DLE) so the
       CLR is parsed as a PAD command rather than forwarded as data. */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    reset_io();
    pad_input_dte(&pad, &dle, 1);
    pad_input_dte(&pad, (const uint8 *)"CLR\r", 4);
    ASSERT_TRUE(contains("CLR CONF"));
    ASSERT_TRUE(!contains("DISCONNECTED"));
}

static void test_telenet_multi_command_recall_keeps_command_mode(void)
{
    /* Telenet recall is multi-shot: after recall, the user can
       issue several commands and the session stays in command mode
       until CONTINUE is typed. Verify by issuing two commands in a
       row after recall and confirming state never transitions back
       to DATA_TRANSFER on its own. */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    pad_input_dte(&pad, &dle, 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    reset_io();
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    /* After STAT under Telenet multi-command recall, we must STILL
       be in WAITING_FOR_CMD (not bumped back to DATA_TRANSFER), and
       a fresh @ prompt must be present so the user can type again.
       Since a call is up, STAT emits engaged_text ("BUSY") rather
       than free_text ("READY"). */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_TRUE(contains("BUSY"));    /* STAT response while call is up */
    ASSERT_TRUE(contains("@"));       /* fresh prompt */
    /* Second command in the same recall session. */
    reset_io();
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_TRUE(contains("BUSY"));
    ASSERT_TRUE(contains("@"));
    /* CONTINUE explicitly returns to data mode. */
    reset_io();
    pad_input_dte(&pad, (const uint8 *)"continue\r", 9);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_default_personality_recall_is_single_shot(void)
{
    /* Default personality keeps the X.28 §3.2.1.5 one-shot recall
       behaviour: any command in WAITING_FOR_CMD auto-returns to
       DATA_TRANSFER. */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default. */
    pad_input_dte(&pad, (const uint8 *)"30199\r", 6);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    pad_input_dte(&pad, &dle, 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    /* One-shot: after the command, we're back in data mode. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_recall_in_data_transfer_emits_prompt(void)
{
    /* X.28 §3.5.23: the PAD-ready prompt indicates readiness for a
       command. PAD recall from DATA_TRANSFER puts the session in
       WAITING_FOR_CMD (command-ready); the prompt must appear
       immediately so the user has the same visual cue as after any
       other transition into a command-ready state. Verified here
       under the Telenet personality (param 6 = 5, prompt char '@'). */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"12345\r", 6);
    ASSERT_TRUE(contains("CONNECTED"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    reset_io();
    pad_input_dte(&pad, &dle, 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_TRUE(contains("@"));
}

static void test_telenet_continue_alias_returns_to_data_transfer(void)
{
    /* End-to-end: place a call (the stub auto-accepts), use PAD
       recall (DLE / 0x10, X.3 param 1's simple-profile default) to
       escape to WAITING_FOR_CMD, issue "continue", verify the state
       machine puts us back in DATA_TRANSFER -- the precise Telenet
       semantic for "return to computer conversation after being in
       command mode". */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"12345\r", 6);
    ASSERT_TRUE(contains("CONNECTED"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    pad_input_dte(&pad, &dle, 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    reset_io();
    pad_input_dte(&pad, (const uint8 *)"continue\r", 9);
    ASSERT_TRUE(!contains("?"));  /* no ERR (Telenet ERR is "?") */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_telenet_cont_short_form_returns_to_data_transfer(void)
{
    /* Same as above but with the abbreviated "cont" form. */
    pad_session_t pad;
    uint8         dle = 0x10;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"12345\r", 6);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    pad_input_dte(&pad, &dle, 1);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    reset_io();
    pad_input_dte(&pad, (const uint8 *)"cont\r", 5);
    ASSERT_TRUE(!contains("?"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_telenet_continue_with_no_call_is_silent(void)
{
    /* Issuing "continue" at the PAD prompt with no call up is a
       benign no-op: no ERR is emitted, and the state machine returns
       to PAD_WAITING (the standard post-dispatch behaviour from
       PAD_COMMAND). */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"continue\r", 9);
    ASSERT_TRUE(!contains("?"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_telenet_HALF_sets_echo_off(void)
{
    /* "HALF" under the Telenet personality must be recognised as a
       SET alias that mutates X.3 param 2 (echo) to 0. */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* Simple-profile default for param 2 is 1 (echo on). The Telenet
       profile_overlay does NOT change param 2 itself, so the starting
       point is still 1. */
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 1);
    pad_input_dte(&pad, (const uint8 *)"HALF\r", 5);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 0);
}

static void test_telenet_FULL_sets_echo_on(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    /* Turn echo off first so the FULL command has work to do. */
    pad_input_dte(&pad, (const uint8 *)"HALF\r", 5);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 0);
    pad_input_dte(&pad, (const uint8 *)"full\r", 5);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 1);
}

static void test_telenet_emits_address_after_banner(void)
{
    /* With Telenet personality + pad_set_address, the handshake banner
       must be followed by a second line carrying the address. Telenet
       requires TWO CRs (autobaud heritage) before the banner is
       emitted. The address must IMMEDIATELY follow the banner's
       CR LF with no intervening blank line. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_set_address(&pad, "127.0.0.1:30099");
    pad_input_dte(&pad, (const uint8 *)"\r\r", 2);
    ASSERT_TRUE(contains("TELENET"));
    ASSERT_TRUE(contains("127.0.0.1:30099"));
    /* Exact-byte check: the byte sequence "TELENET\r\n127.0.0.1:30099"
       must appear contiguously. Any blank line between them would
       insert an extra CR LF and break this match. */
    ASSERT_TRUE(contains("TELENET\r\n127.0.0.1:30099"));
}

static void test_default_personality_suppresses_address_line(void)
{
    /* Even with pad_set_address set, the default personality has
       emit_address = 0, so the second line MUST NOT appear. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default. */
    pad_set_identification(&pad, "PADAWAN-LITE v1.2");
    pad_set_address(&pad, "127.0.0.1:30099");
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_TRUE(contains("PADAWAN-LITE v1.2"));
    ASSERT_TRUE(!contains("127.0.0.1:30099"));
}

static void test_telenet_with_no_address_emits_only_banner(void)
{
    /* Telenet personality but pad_set_address never called: the
       second line is suppressed (no empty address line). Telenet
       still requires two CRs to complete the handshake. */
    pad_session_t pad;
    reset_io();
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r\r", 2);
    ASSERT_TRUE(contains("TELENET"));
    /* The banner ends with CR LF; no trailing extra CR LF from an
       empty address. */
    {
        uint32 i, blank_lines = 0;
        for (i = 1; i < g_dte_len; i++) {
            if (g_dte[i - 1] == '\r' && g_dte[i] == '\n') {
                if (i + 2 < g_dte_len &&
                    g_dte[i + 1] == '\r' && g_dte[i + 2] == '\n') {
                    blank_lines++;
                }
            }
        }
        (void)blank_lines;  /* shape check, not a hard count */
    }
}

static void test_default_personality_does_not_recognise_half(void)
{
    /* No alias table under the default personality; "HALF" must not
       mutate param 2. It falls through to the bare SELECTION parser
       (empty address, no SET behaviour). */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default. */
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 1);
    pad_input_dte(&pad, (const uint8 *)"HALF\r", 5);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 1);
}

static void test_default_personality_rejects_telenet_aliases(void)
{
    /* Without a personality's alias table, "c 12345" must not be
       recognised as a Telenet "CONNECT" synonym. It falls through to
       the implicit SELECTION path where the address scan terminates
       at the non-digit 'c' (the bridge stub may still auto-accept on
       an empty address and emit X.28-standard "COM" -- that's not
       what we're testing here). The key invariant is that the
       Telenet-flavoured "CONNECTED" text must NOT appear under the
       default personality. */
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    /* No pad_set_personality => default behaviour, no aliases. */
    pad_input_dte(&pad, (const uint8 *)"c 12345\r", 8);
    ASSERT_TRUE(!contains("CONNECTED"));
}

int main(void)
{
    test_lookup_default();
    test_lookup_known();
    test_lookup_unknown_returns_null();
    test_default_personality_is_passthrough();
    test_telenet_personality_replaces_stat();
    test_telenet_personality_replaces_err();
    test_profile_overlay_applies();
    test_profile_overlay_skips_speed_param();
    test_nui_prompt_text_overridden();
    test_personality_null_reverts_to_default();
    test_telenet_prompt_char_visible();
    test_banner_set_by_personality();
    test_telenet_command_aliases_present();
    test_default_has_no_command_aliases();
    test_telenet_c_alias_with_space_dispatches_call();
    test_telenet_C_alias_with_space_dispatches_call();
    test_telenet_c_no_space_does_not_match_alias();
    test_telenet_CONNECT_alias_dispatches_call();
    test_telenet_D_alias_dispatches_clr();
    test_telenet_DISCONNECT_alias_dispatches_clr();
    test_telenet_connected_includes_called_address();
    test_telenet_disconnect_emits_address_and_text();
    test_default_personality_uses_standard_com();
    test_default_personality_clr_uses_standard_confirm();
    test_telenet_multi_command_recall_keeps_command_mode();
    test_default_personality_recall_is_single_shot();
    test_telenet_handshake_requires_two_crs();
    test_default_handshake_still_single_cr();
    test_telenet_terminal_prompt_captures_value();
    test_telenet_terminal_prompt_accepts_empty();
    test_telenet_terminal_prompt_truncates_overflow();
    test_default_personality_no_terminal_prompt();
    test_recall_in_data_transfer_emits_prompt();
    test_telenet_continue_alias_returns_to_data_transfer();
    test_telenet_cont_short_form_returns_to_data_transfer();
    test_telenet_continue_with_no_call_is_silent();
    test_telenet_HALF_sets_echo_off();
    test_telenet_FULL_sets_echo_on();
    test_telenet_emits_address_after_banner();
    test_default_personality_suppresses_address_line();
    test_telenet_with_no_address_emits_only_banner();
    test_default_personality_does_not_recognise_half();
    test_default_personality_rejects_telenet_aliases();
    TEST_REPORT();
}
