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

/* Tests for the PAD state machine and command dispatcher. */
#include "test.h"
#include "pad.h"
#include "x3.h"
#include "x28_signals.h"
#include "x29.h"
#include "x25_stub.h"
#include <string.h>

#define IA5_CR 0x0D

typedef struct {
    uint8  dte_out[2048];
    uint32 dte_len;
    uint8  remote_out[2048];
    uint32 remote_len;
} test_io_t;

static void cb_dte(void *ctx, const uint8 *data, uint32 len)
{
    test_io_t *io = (test_io_t *)ctx;
    if (io->dte_len + len <= sizeof(io->dte_out)) {
        memcpy(io->dte_out + io->dte_len, data, len);
        io->dte_len += len;
    }
}

static void cb_remote(void *ctx, const uint8 *data, uint32 len)
{
    test_io_t *io = (test_io_t *)ctx;
    if (io->remote_len + len <= sizeof(io->remote_out)) {
        memcpy(io->remote_out + io->remote_len, data, len);
        io->remote_len += len;
    }
}

static int contains(const uint8 *hay, uint32 hlen, const char *needle)
{
    uint32 nlen = (uint32)strlen(needle);
    uint32 i;
    if (nlen > hlen) return 0;
    for (i = 0; i + nlen <= hlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static void feed(pad_session_t *p, const char *s)
{
    pad_input_dte(p, (const uint8 *)s, (uint32)strlen(s));
}

static void feed_byte(pad_session_t *p, uint8 b)
{
    pad_input_dte(p, &b, 1);
}

/* --- tests ------------------------------------------------------------- */

static void test_init_state(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    ASSERT_EQ_INT(pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io), 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!pad.call.connected);
}

static void test_stat_idle_reports_free(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "STAT\r");
    /* X.28 3.5.11: no call -> FREE service signal. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "FREE"));
}

static void test_par_q_reads_simple_profile(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "PAR? 2\r");
    /* Simple profile param 2 = 1. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "2:1"));
}

static void test_set_then_par_reflects_new_value(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 2:0\r");   /* echo off after this */
    io.dte_len = 0;
    feed(&pad, "PAR? 2\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "2:0"));
}

static void test_set_invalid_value_emits_par_inv(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 v;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* X.3 3.1: recall does not accept 5. X.28 3.3.2 + 3.5.14: the invalid
       pair must be reported via "INV" in a PAR response, not ERR. */
    feed(&pad, "SET 1:5\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "1:INV"));
    /* Invalid pair must not be applied: param 1 keeps the profile default. */
    x3_get(&pad.params, X3_PAR_RECALL, &v);
    ASSERT_EQ_INT(v, 1);
}

static void test_set_partial_accept(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 v;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* X.28 3.3.2: valid pairs in a mixed batch are accepted; invalid ones
       are reported via PAR/INV. */
    feed(&pad, "SET 1:5, 2:0\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "1:INV"));
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "2:0"));
    /* The valid pair (param 2 = 0) was applied. */
    x3_get(&pad.params, X3_PAR_ECHO, &v);
    ASSERT_EQ_INT(v, 0);
    /* The invalid pair was not applied; param 1 stayed at profile default. */
    x3_get(&pad.params, X3_PAR_RECALL, &v);
    ASSERT_EQ_INT(v, 1);
}

static void test_clr_emits_confirmation(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);
    io.dte_len = 0;
    feed(&pad, "CLR\r");
    /* §3.5.9 + Table 7/X.28: "CLR CONF" + format effector. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR CONF"));
}

static void test_forwarding_alphanumeric(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Switch param 3 to alphanumeric-only (bit 0 = value 1). */
    feed(&pad, "SET 3:1\r");
    feed(&pad, "12345\r");                           /* enter data transfer */
    io.remote_len = 0;
    /* In data transfer, a single alphanumeric should now trigger forward. */
    feed_byte(&pad, 'A');
    ASSERT_EQ_INT(io.remote_len, 1);
    ASSERT_EQ_INT(io.remote_out[0], 'A');
}

static void test_forwarding_esc(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 3:4\r");                          /* bit 2 = ESC/BEL/ENQ/ACK */
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed_byte(&pad, 0x1B);                            /* ESC */
    ASSERT_EQ_INT(io.remote_len, 1);
    ASSERT_EQ_INT(io.remote_out[0], 0x1B);
}

static void test_forwarding_disabled(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 3:0\r");                          /* no forwarding chars */
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed(&pad, "ABC\rDEF\r");                         /* nothing should forward */
    ASSERT_EQ_INT(io.remote_len, 0);
}

static void test_echo_scrambled(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Set echo to scrambled '*' (decimal 42). */
    feed(&pad, "SET 2:42\r");
    io.dte_len = 0;
    feed_byte(&pad, 'X');
    /* X is captured into the edit buffer (PAD waiting state) but echoed as '*'. */
    ASSERT_EQ_INT(io.dte_len, 1);
    ASSERT_EQ_INT(io.dte_out[0], '*');
}

static void test_echo_except_forwarding(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* echo = 2 (echo all except data forwarding chars). With simple
       profile's param 3 = 126, CR is a forwarding char. */
    feed(&pad, "SET 2:2\r");
    io.dte_len = 0;
    feed_byte(&pad, 'A');                             /* not forwarding -> echoed */
    feed_byte(&pad, IA5_CR);                          /* forwarding char -> not echoed */
    /* Only 'A' should have been echoed. The CR went into the edit buffer
       and triggered parse, but echo_byte must have suppressed its emit. */
    ASSERT_EQ_INT(io.dte_out[0], 'A');
}

static void test_cdel_emits_backslash(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 input[3];
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Simple profile: param 19 = 1, param 16 = 127 (DEL). Type 'A' + DEL.
       Per §3.5.24: param 19 = 1 -> emit "\". */
    input[0] = 'A'; input[1] = 0x7F;
    pad_input_dte(&pad, input, 2);
    /* dte_out should be: 'A' (echo of A) then '\' (cdel signal). */
    ASSERT_EQ_INT(io.dte_len, 2);
    ASSERT_EQ_INT(io.dte_out[0], 'A');
    ASSERT_EQ_INT(io.dte_out[1], '\\');
}

static void test_ldel_emits_xxx(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 input[4];
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Simple profile: param 19 = 1, param 17 = 24 (CAN). Type 'A' 'B' + CAN.
       Per §3.5.25 with param 19 = 1: emit "XXX" + format effector. */
    input[0] = 'A'; input[1] = 'B'; input[2] = 0x18;
    pad_input_dte(&pad, input, 3);
    /* dte_out: echo "AB" then "XXX\r\n". */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "XXX\r\n"));
}

static void test_prof_loads_transparent(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 v;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "PROF 91\r");
    x3_get(&pad.params, X3_PAR_ECHO, &v);
    /* Transparent profile has echo = 0 (X.28 Table 1). */
    ASSERT_EQ_INT(v, 0);
}

static void test_selection_starts_call_and_enters_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
}

static void test_stat_after_call_reports_engaged(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);                            /* DLE = PAD recall */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    io.dte_len = 0;
    feed(&pad, "STAT\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "ENGAGED"));
    /* After STAT in waiting-for-cmd state, the PAD returns to data
       transfer because the call is still up. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_data_transfer_forwards_on_cr(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.remote_len = 0;
    /* Simple profile param 3 = 126; bit 1 (value 2) is set, so CR forwards. */
    feed(&pad, "HELLO\r");
    ASSERT_EQ_INT(pad.asm_len, 0);
    ASSERT_EQ_INT(io.remote_len, 6); /* H E L L O CR */
    ASSERT_MEM_EQ(io.remote_out, "HELLO\r", 6);
}

static void test_clr_tears_down_call(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);
    feed(&pad, "CLR\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!pad.call.connected);
}

/* Auth callback that allows only one specific NUI. ctx points to the
   expected NUI string. Returns 0 on match, -1 otherwise. */
static int auth_cb_only_alice(void *ctx,
                              const struct x28_facility_t *facilities,
                              uint8 facility_count,
                              const char *address,
                              const char *session_nui)
{
    const char *expect = (const char *)ctx;
    uint8 i;
    (void)address;
    for (i = 0; i < facility_count; i++) {
        if (facilities[i].code == 'N' &&
            strcmp(facilities[i].arg, expect) == 0) return 0;
    }
    if (session_nui != NULL && strcmp(session_nui, expect) == 0) return 0;
    return -1;
}

static void test_auth_callback_allows_matching_nui(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_auth_callback(&pad, auth_cb_only_alice, (void *)"alice");
    feed(&pad, "Nalice-12345\r");
    /* Call should proceed: facility 'N' = "alice" matches the gate. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
}

static void test_auth_callback_rejects_with_clr_na(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_auth_callback(&pad, auth_cb_only_alice, (void *)"alice");
    /* Wrong NUI -> reject. */
    feed(&pad, "Nbob-12345\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR NA"));
    ASSERT_TRUE(!pad.call.connected);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_auth_callback_rejects_when_no_nui(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_auth_callback(&pad, auth_cb_only_alice, (void *)"alice");
    /* Bare selection - no facility block, no NUI -> reject. */
    feed(&pad, "12345\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR NA"));
    ASSERT_TRUE(!pad.call.connected);
}

static void test_id_command_sets_session_nui(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID alice\r");
    ASSERT_EQ_INT(strcmp(pad.session_nui, "alice"), 0);
}

static void test_idoff_clears_session_nui(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID alice\r");
    feed(&pad, "IDOFF\r");
    ASSERT_EQ_INT(pad.session_nui[0], '\0');
}

static void test_session_nui_satisfies_auth_gate(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_auth_callback(&pad, auth_cb_only_alice, (void *)"alice");
    /* X.28 §5.2: ID sets session-level NUI; subsequent bare SELECTION
       inherits it and passes the auth gate. */
    feed(&pad, "ID alice\r");
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
}

static void test_idoff_revokes_auth_for_next_call(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_auth_callback(&pad, auth_cb_only_alice, (void *)"alice");
    feed(&pad, "ID alice\r");
    feed(&pad, "IDOFF\r");
    feed(&pad, "12345\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR NA"));
    ASSERT_TRUE(!pad.call.connected);
}

static void test_pending_dte_replayed_on_call_connected(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    /* Force async setup so SELECTION leaves us in CONN_IN_PROGRESS. */
    x25_stub_set_async(1);
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    /* Type bytes during call setup - spec says drop, Padawan-Lite buffers. */
    feed(&pad, "hello");
    ASSERT_EQ_INT(pad.pending_dte_len, 5);
    /* Complete the connection; pending bytes get replayed and forwarded. */
    pad_call_connected(&pad);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_EQ_INT(pad.pending_dte_len, 0);
    /* "hello" should have arrived at the remote (no forwarding char in
       transparent profile, so check the assembly buffer). */
    ASSERT_TRUE(pad.asm_len == 5 && memcmp(pad.asm_buf, "hello", 5) == 0);
    x25_stub_set_async(0);
}

static void test_pending_dte_dropped_on_recall(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    x25_stub_set_async(1);
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    /* Transparent profile has recall char = DLE (0x10). Wait - actually
       transparent has param 1 = 0 (no recall). Use simple profile but
       force async via the stub setting. */
    x25_stub_set_async(0);
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    feed(&pad, "abc");
    ASSERT_EQ_INT(pad.pending_dte_len, 3);
    feed_byte(&pad, 0x10);            /* DLE -> recall, escapes to waiting */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_EQ_INT(pad.pending_dte_len, 0);
    x25_stub_set_async(0);
}

static void test_lf_after_cr_in_dte_input(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                         /* into data transfer */
    io.remote_len = 0;
    /* X.3 3.13 bit 1 (value 2): insert LF after every CR in input. */
    pad.params.values[X3_PAR_LF_INSERT] = 0x02;
    pad.params.values[X3_PAR_FORWARD]   = 0x02;    /* forward on CR */
    feed_byte(&pad, IA5_CR);
    /* Remote should see CR + LF as a pair, not bare CR. */
    ASSERT_TRUE(io.remote_len >= 2);
    ASSERT_EQ_INT(io.remote_out[io.remote_len - 2], IA5_CR);
    ASSERT_EQ_INT(io.remote_out[io.remote_len - 1], 0x0A);
}

static void test_qualified_remote_data_dropped(void)
{
    test_io_t io;
    pad_session_t pad;
    /* A non-X.29-typed PAD message (byte 0 = 'P' = 0x50, unrecognised)
       is not delivered to the DTE; the PAD instead replies with an
       Error PAD message to the remote. */
    uint8 garbage[] = { 0x50, 'A', 'D', 'M' };
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_input_remote(&pad, garbage, sizeof(garbage), 1);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_x29_invite_clear_tears_down_call(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 msg[] = { 0x01 };           /* X.29 Invitation to clear */
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    ASSERT_TRUE(pad.call.connected);
    pad_input_remote(&pad, msg, sizeof(msg), 1);
    ASSERT_TRUE(!pad.call.connected);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR"));
}

static void test_x29_set_updates_local_params(void)
{
    test_io_t io;
    pad_session_t pad;
    /* X.29 Set, two pairs: (2,0) and (3,0). */
    uint8 msg[] = { 0x02, 2, 0, 3, 0 };
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 1);    /* simple default */
    ASSERT_EQ_INT(pad.params.values[X3_PAR_FORWARD], 126);
    pad_input_remote(&pad, msg, sizeof(msg), 1);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_ECHO], 0);
    ASSERT_EQ_INT(pad.params.values[X3_PAR_FORWARD], 0);
}

static void test_x29_parameter_indication_emits_par(void)
{
    test_io_t io;
    pad_session_t pad;
    /* X.29 PI: ref 2 = 0, ref 3 = 126. Should produce a PAR signal
       on the DTE side. */
    uint8 msg[] = { 0x00, 2, 0, 3, 126 };
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_input_remote(&pad, msg, sizeof(msg), 1);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "PAR"));
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "2:0"));
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "3:126"));
}

static void test_x29_break_emits_bel_and_param8(void)
{
    test_io_t io;
    pad_session_t pad;
    /* X.29 Indication of break with the pair form {8, 1} - set
       discard. */
    uint8 msg[] = { 0x03, 8, 1 };
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_input_remote(&pad, msg, sizeof(msg), 1);
    ASSERT_TRUE(io.dte_len > 0);
    ASSERT_EQ_INT(io.dte_out[0], 0x07);                  /* BEL */
    ASSERT_EQ_INT(pad.params.values[X3_PAR_DISCARD], 1); /* param 8 set */
}

static void test_clr_indication_includes_cause_code(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                  /* place a call */
    io.dte_len = 0;
    pad_remote_cleared(&pad, PAD_CLR_NUMBER_NOT_ASSIGNED, 13, 0x6F);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR NP C:13"));
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "D:111"));
}

static void test_id_with_no_arg_emits_nui_prompt(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "NUI?"));
    ASSERT_EQ_INT(pad.awaiting_nui, 1);
}

static void test_id_prompt_captures_next_input_as_nui(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID\r");
    feed(&pad, "david\r");
    ASSERT_EQ_INT(pad.awaiting_nui, 0);
    ASSERT_EQ_INT(strcmp(pad.session_nui, "david"), 0);
}

static void test_id_prompt_empty_answer_leaves_nui_unchanged(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID alice\r");
    feed(&pad, "ID\r");
    feed(&pad, "\r");
    ASSERT_EQ_INT(pad.awaiting_nui, 0);
    ASSERT_EQ_INT(strcmp(pad.session_nui, "alice"), 0);
}

typedef struct {
    int   dte_dir_calls;
    int   remote_dir_calls;
    uint8 last_buf[64];
    uint32 last_len;
    int   last_dir;
} trace_record_t;

static void test_trace_cb(void *ctx, int direction,
                          const uint8 *data, uint32 len)
{
    trace_record_t *r = (trace_record_t *)ctx;
    if (direction == PAD_TRACE_DTE)    r->dte_dir_calls++;
    if (direction == PAD_TRACE_REMOTE) r->remote_dir_calls++;
    r->last_dir = direction;
    r->last_len = len;
    if (len <= sizeof(r->last_buf)) memcpy(r->last_buf, data, len);
}

static void test_trace_callback_fires_for_dte_input(void)
{
    test_io_t io;
    pad_session_t pad;
    trace_record_t rec;
    memset(&io, 0, sizeof(io));
    memset(&rec, 0, sizeof(rec));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_trace_callback(&pad, test_trace_cb, &rec);
    feed(&pad, "STAT\r");
    /* feed() makes one pad_input_dte call -> exactly one trace fire. */
    ASSERT_EQ_INT(rec.dte_dir_calls, 1);
    ASSERT_EQ_INT(rec.remote_dir_calls, 0);
    ASSERT_EQ_INT(rec.last_dir, PAD_TRACE_DTE);
    ASSERT_EQ_INT(rec.last_len, 5);
    ASSERT_EQ_INT(memcmp(rec.last_buf, "STAT\r", 5), 0);
}

static void test_trace_callback_fires_for_remote_input(void)
{
    test_io_t io;
    pad_session_t pad;
    trace_record_t rec;
    memset(&io, 0, sizeof(io));
    memset(&rec, 0, sizeof(rec));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_trace_callback(&pad, test_trace_cb, &rec);
    feed(&pad, "12345\r");                  /* enter data transfer */
    pad_input_remote(&pad, (const uint8 *)"HELLO", 5, 0);
    ASSERT_EQ_INT(rec.remote_dir_calls, 1);
    ASSERT_EQ_INT(rec.last_dir, PAD_TRACE_REMOTE);
    ASSERT_EQ_INT(rec.last_len, 5);
    ASSERT_EQ_INT(memcmp(rec.last_buf, "HELLO", 5), 0);
}

static void test_trace_callback_silent_on_zero_len(void)
{
    test_io_t io;
    pad_session_t pad;
    trace_record_t rec;
    memset(&io, 0, sizeof(io));
    memset(&rec, 0, sizeof(rec));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_trace_callback(&pad, test_trace_cb, &rec);
    pad_input_dte(&pad, NULL, 0);
    pad_input_remote(&pad, NULL, 0, 0);
    /* Zero-length calls must not invoke the trace. */
    ASSERT_EQ_INT(rec.dte_dir_calls, 0);
    ASSERT_EQ_INT(rec.remote_dir_calls, 0);
}

static void test_lf_not_inserted_when_bit_clear(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.remote_len = 0;
    pad.params.values[X3_PAR_LF_INSERT] = 0x00;    /* bit 1 not set */
    pad.params.values[X3_PAR_FORWARD]   = 0x02;
    feed_byte(&pad, IA5_CR);
    /* Remote should see bare CR (no LF inserted). */
    ASSERT_EQ_INT(io.remote_out[io.remote_len - 1], IA5_CR);
}

static void test_pending_dte_dropped_on_clear(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    x25_stub_set_async(1);
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed(&pad, "abc");
    ASSERT_EQ_INT(pad.pending_dte_len, 3);
    pad_remote_cleared(&pad, PAD_CLR_NETWORK_PROBLEM, 0, 0);
    ASSERT_EQ_INT(pad.pending_dte_len, 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    x25_stub_set_async(0);
}

static void test_clr_when_idle_returns_err(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "CLR\r");
    /* X.28 3.2.3.1.3: clear request while in PAD waiting state is invalid. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "ERR"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_int_when_idle_returns_err(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "INT\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "ERR"));
}

static void test_editing_cdel_removes_last_char(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 input[8];
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Simple profile param 16 (cdel) = 127 (DEL).
       Type "STAX" + DEL + "T" + CR -> buffer "STAT" -> parses to STAT. */
    input[0] = 'S'; input[1] = 'T'; input[2] = 'A'; input[3] = 'X';
    input[4] = 0x7F;
    input[5] = 'T'; input[6] = '\r';
    pad_input_dte(&pad, input, 7);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "FREE"));
}

static void test_editing_ldel_clears_buffer(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 input[10];
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Simple profile param 17 (ldel) = 24 (CAN). Type garbage + CAN +
       STAT + CR -> ldel wipes garbage; STAT parses. */
    input[0] = 'G'; input[1] = 'X'; input[2] = 'Q';
    input[3] = 0x18;
    input[4] = 'S'; input[5] = 'T'; input[6] = 'A'; input[7] = 'T'; input[8] = '\r';
    pad_input_dte(&pad, input, 9);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "FREE"));
}

static void test_editing_ldis_redisplays_buffer(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 input[6];
    uint32 dte_before;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Simple profile param 18 (ldis) = 18 (DC2). After typing "ST" send
       DC2: ldis re-echoes "ST". */
    input[0] = 'S'; input[1] = 'T';
    pad_input_dte(&pad, input, 2);
    dte_before = io.dte_len;
    input[0] = 0x12; /* DC2 */
    pad_input_dte(&pad, input, 1);
    /* Two more bytes echoed by ldis. */
    ASSERT_EQ_INT(io.dte_len, dte_before + 2);
    ASSERT_EQ_INT(io.dte_out[io.dte_len - 2], 'S');
    ASSERT_EQ_INT(io.dte_out[io.dte_len - 1], 'T');
}

static void test_remote_data_forwarded_to_dte_in_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    const uint8 payload[5];
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    memcpy((void *)payload, "WORLD", 5);
    pad_input_remote(&pad, payload, 5, 0);
    ASSERT_EQ_INT(io.dte_len, 5);
    ASSERT_MEM_EQ(io.dte_out, "WORLD", 5);
}

static void test_remote_data_dropped_outside_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_input_remote(&pad, (const uint8 *)"XYZ", 3, 0);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_idle_timer_disabled_when_param4_zero(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    io.remote_len = 0;
    feed_byte(&pad, 'X');                             /* not a forwarding char */
    pad_tick(&pad, 1000);                             /* lots of time */
    /* X.3 3.4: param 4 = 0 disables the idle timer; nothing forwards. */
    ASSERT_EQ_INT(io.remote_len, 0);
    ASSERT_EQ_INT(pad.asm_len, 1);
}

static void test_idle_timer_flushes_after_threshold(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Set idle = 2 twentieths (100 ms). */
    feed(&pad, "SET 4:2\r");
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed(&pad, "AB");                                 /* no forwarding chars */
    ASSERT_EQ_INT(io.remote_len, 0);
    pad_tick(&pad, 1);                                /* below threshold */
    ASSERT_EQ_INT(io.remote_len, 0);
    pad_tick(&pad, 1);                                /* reaches threshold */
    ASSERT_EQ_INT(io.remote_len, 2);
    ASSERT_MEM_EQ(io.remote_out, "AB", 2);
    ASSERT_EQ_INT(pad.asm_len, 0);
}

static void test_idle_timer_resets_on_byte(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 4:4\r");                          /* 4 twentieths */
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed_byte(&pad, 'A');
    pad_tick(&pad, 3);                                /* below threshold */
    feed_byte(&pad, 'B');                             /* resets idle_ticks */
    pad_tick(&pad, 3);                                /* still below */
    ASSERT_EQ_INT(io.remote_len, 0);
    pad_tick(&pad, 1);                                /* now at 4 */
    ASSERT_EQ_INT(io.remote_len, 2);
}

static void test_idle_timer_noop_outside_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 4:1\r");
    /* Still in PAD_WAITING. pad_tick must not flush anything. */
    pad_tick(&pad, 1000);
    ASSERT_EQ_INT(io.remote_len, 0);
}

static void test_break_bit_3_escapes_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 7:8\r");                          /* bit 3: escape */
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    pad_break(&pad);
    /* X.3 3.7 bit 3 + X.28 4.11: escape from data transfer state. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
}

static void test_break_bit_4_discards_remote_output(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 v;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 7:16\r");                         /* bit 4: discard output */
    feed(&pad, "12345\r");
    pad_break(&pad);
    /* Bit 4 sets X.3 param 8 = 1. */
    x3_get(&pad.params, X3_PAR_DISCARD, &v);
    ASSERT_EQ_INT(v, 1);
    /* Remote bytes are now discarded. */
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"HELLO", 5, 0);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_break_param_7_zero_is_noop(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 7:0\r");
    feed(&pad, "12345\r");
    pad_break(&pad);
    /* No bits set: state and discard flag unchanged. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_extended_status_alias(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Table 9/X.28: STATUS is the extended keyword for STAT. */
    feed(&pad, "STATUS\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "FREE"));
}

static void test_extended_help_emits_help_signal(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "HELP\r");
    /* §5.5: a help PAD service signal is emitted. v1.1 begins with "HELP". */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "HELP"));
}

static void test_extended_break_does_not_escape(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 7:8\r");                          /* param 7 bit 3: escape */
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);                            /* DLE -> WAITING_FOR_CMD */
    feed(&pad, "BREAK\r");
    /* §5.1 NOTE: escape from data transfer is not possible via BREAK.
       After dispatch we should return to DATA_TRANSFER per the normal
       post-command-in-WAITING_FOR_CMD flow, not stay escaped. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_extended_nui_on_off_acknowledge(void)
{
    test_io_t io;
    pad_session_t pad;
    uint32 baseline;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "ID some-user\r");
    /* No ERR for valid command. */
    ASSERT_TRUE(!contains(io.dte_out, io.dte_len, "ERR"));
    baseline = io.dte_len;
    feed(&pad, "IDOFF\r");
    ASSERT_TRUE(io.dte_len > baseline);
    ASSERT_TRUE(!contains(io.dte_out + baseline, io.dte_len - baseline, "ERR"));
}

static void test_cr_padding_inserts_nuls(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* X.3 3.9 / X.28 4.12: 3 NUL pad chars after every CR sent to DTE. */
    feed(&pad, "SET 9:3\r");
    io.dte_len = 0;
    feed_byte(&pad, IA5_CR);
    /* CR echoed (1) + 3 NULs. */
    ASSERT_EQ_INT(io.dte_len, 4);
    ASSERT_EQ_INT(io.dte_out[0], 0x0D);
    ASSERT_EQ_INT(io.dte_out[1], 0x00);
    ASSERT_EQ_INT(io.dte_out[2], 0x00);
    ASSERT_EQ_INT(io.dte_out[3], 0x00);
}

static void test_lf_padding_only_in_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Enter data transfer, then set param 14 = 2 directly. */
    feed(&pad, "12345\r");
    pad.params.values[X3_PAR_LF_PAD] = 2;
    io.dte_len = 0;
    /* A remote LF must be padded with 2 NULs in data transfer state. */
    pad_input_remote(&pad, (const uint8 *)"X\nY", 3, 0);
    ASSERT_EQ_INT(io.dte_len, 5);
    ASSERT_MEM_EQ(io.dte_out, "X\n\0\0Y", 5);
}

static void test_editing_in_data_transfer_when_param15_one(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 cdel = 0x7F;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 15:1\r");
    feed(&pad, "12345\r");
    io.remote_len = 0;
    /* Type "AX" then DEL: in data transfer with editing on, DEL removes
       the X. Then forwarding char CR flushes "A". */
    feed_byte(&pad, 'A');
    feed_byte(&pad, 'X');
    pad_input_dte(&pad, &cdel, 1);
    feed_byte(&pad, IA5_CR);
    /* Forwarded buffer should contain "A\r" (X was deleted). */
    ASSERT_EQ_INT(io.remote_len, 2);
    ASSERT_MEM_EQ(io.remote_out, "A\r", 2);
}

static void test_editing_in_data_transfer_off_by_default(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 cdel = 0x7F;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Default param 15 = 0; DEL must be passed through as data. */
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed_byte(&pad, 'A');
    pad_input_dte(&pad, &cdel, 1);
    feed_byte(&pad, IA5_CR);
    /* Forwarded should include the DEL byte (no editing). */
    ASSERT_EQ_INT(io.remote_len, 3);
    ASSERT_MEM_EQ(io.remote_out, "A\x7F\r", 3);
}

static void test_idle_timer_suspended_when_editing_in_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 4:2, 15:1\r");                    /* idle=2 + editing on */
    feed(&pad, "12345\r");
    io.remote_len = 0;
    feed(&pad, "AB");
    pad_tick(&pad, 1000);
    /* X.3 3.15: idle forwarding is suspended when editing is on. */
    ASSERT_EQ_INT(io.remote_len, 0);
}

static void test_echo_mask_suppresses_cr(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* X.3 3.20 bit 0 (value 1): no echo of CR. Feed CR with an empty edit
       buffer so the dispatch path doesn't emit anything either. */
    feed(&pad, "SET 20:1\r");
    io.dte_len = 0;
    feed_byte(&pad, IA5_CR);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_echo_mask_suppresses_editing_chars(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 cdel = 0x7F;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Bit 6 (value 64): no echo of editing chars (cdel/ldel/ldis). */
    feed(&pad, "SET 20:64\r");
    io.dte_len = 0;
    feed_byte(&pad, 'A');
    pad_input_dte(&pad, &cdel, 1);                    /* editing - should also not echo */
    /* DEL went through editing (removed A from buffer); the cdel signal
       "\" is still emitted via emit_cdel_signal. So output is 'A' + '\\'
       and no echo of the DEL byte itself. */
    ASSERT_EQ_INT(io.dte_len, 2);
    ASSERT_EQ_INT(io.dte_out[0], 'A');
    ASSERT_EQ_INT(io.dte_out[1], '\\');
}

static void test_lf_after_cr_echo_when_param13_bit2(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* X.3 3.13 bit 2 (value 4): insert LF after echo of CR. */
    feed(&pad, "SET 13:4\r");
    io.dte_len = 0;
    feed_byte(&pad, IA5_CR);
    /* CR echoed (1), then LF inserted (2). */
    ASSERT_EQ_INT(io.dte_len, 2);
    ASSERT_EQ_INT(io.dte_out[0], 0x0D);
    ASSERT_EQ_INT(io.dte_out[1], 0x0A);
}

static void test_no_lf_after_cr_echo_when_bit2_clear(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Default param 13 = 0; CR echo should NOT be followed by LF. */
    feed_byte(&pad, IA5_CR);
    ASSERT_EQ_INT(io.dte_len, 1);
    ASSERT_EQ_INT(io.dte_out[0], 0x0D);
}

static void test_lf_insertion_in_remote_stream_when_bit0(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    feed(&pad, "SET 13:1\r");                         /* bit 0 in this state */
    /* Wait - SET goes through PAD command; we'd need to escape first.
       Instead, set param directly. */
    pad.params.values[X3_PAR_LF_INSERT] = 1;
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"AB\rCD\r", 6, 0);
    /* Each CR should be followed by an inserted LF. */
    ASSERT_EQ_INT(io.dte_len, 8);
    ASSERT_MEM_EQ(io.dte_out, "AB\r\nCD\r\n", 8);
}

/* ---- params 5, 10, 12, 22 + param 6 prompt --------------------------- */

static void test_param12_dc3_xoffs_output(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    /* Simple profile already has param 12 = 1. DC3 from DTE should
       suspend output to DTE until DC1 arrives. */
    feed_byte(&pad, 0x13);                            /* X-OFF */
    ASSERT_TRUE(pad.xoff_from_dte);
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"hello", 5, 0);
    /* Bytes queued, nothing emitted. */
    ASSERT_EQ_INT(io.dte_len, 0);
    ASSERT_EQ_INT(pad.remote_q_len, 5);
}

static void test_param12_dc1_clears_xoff(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x13);
    pad_input_remote(&pad, (const uint8 *)"hello", 5, 0);
    io.dte_len = 0;
    feed_byte(&pad, 0x11);                            /* X-ON */
    ASSERT_TRUE(!pad.xoff_from_dte);
    /* Queued bytes drain on X-ON. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "hello"));
}

static void test_param12_dc1_dc3_not_echoed(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    feed_byte(&pad, 0x13);                            /* X-OFF: consumed */
    /* While X-OFF, output is suppressed - no echo, no service signal. */
    ASSERT_EQ_INT(io.dte_len, 0);
    feed_byte(&pad, 0x11);                            /* X-ON: consumed */
    /* Still no DC1/DC3 echoed (no asm-buffer storage either). */
    ASSERT_EQ_INT(pad.asm_len, 0);
}

static void test_param12_disabled_treats_dc1_dc3_as_data(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    /* Transparent profile has param 12 = 0; DC1/DC3 are not flow control. */
    pad.call.connected = 1;
    pad.state = PAD_STATE_DATA_TRANSFER;
    feed_byte(&pad, 0x13);
    /* DC3 stored as data (transparent has no echo so we only check asm). */
    ASSERT_EQ_INT(pad.asm_len, 1);
    ASSERT_EQ_INT(pad.asm_buf[0], 0x13);
}

static void test_param22_page_wait_after_n_lfs(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 22:2\r");                         /* page wait after 2 LFs */
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"a\nb\nc\n", 6, 0);
    /* After the 2nd LF, page wait fires and "PAGE" is emitted; the
       trailing "c\n" is dropped. */
    ASSERT_TRUE(pad.page_wait_active);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "PAGE"));
}

static void test_param22_dc1_cancels_page_wait(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    /* Place the session directly in page-wait so the test isn't tangled
       with the LF accumulation a real path would do (which can re-trip
       the wait condition if the queue contains LFs). */
    pad.page_wait_active = 1;
    pad.lf_count = 5;
    io.dte_len = 0;
    feed_byte(&pad, 0x11);                            /* X-ON cancels */
    ASSERT_TRUE(!pad.page_wait_active);
    ASSERT_EQ_INT(pad.lf_count, 0);
    /* §4.18.2: PAD transmits a format effector to resume. Queue is empty,
       so only the effector is emitted. */
    ASSERT_EQ_INT(io.dte_len, 2);
    ASSERT_EQ_INT(io.dte_out[0], 0x0D);
    ASSERT_EQ_INT(io.dte_out[1], 0x0A);
}

static void test_param10_line_folding_inserts_effector(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 10:4\r");                         /* fold at column 4 */
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"ABCDE", 5, 0);
    /* After 4 graphic chars, an inserted format effector precedes E. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "ABCD\r\nE"));
}

static void test_param10_cr_resets_column(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "SET 10:4\r");
    feed(&pad, "12345\r");
    io.dte_len = 0;
    /* "AB\rCD" - CR resets the column counter, so no fold injected. */
    pad_input_remote(&pad, (const uint8 *)"AB\rCD", 5, 0);
    /* Output: AB\rCD (5 bytes, no auto effector). */
    ASSERT_EQ_INT(io.dte_len, 5);
    ASSERT_MEM_EQ(io.dte_out, "AB\rCD", 5);
}

static void test_param5_emits_xoff_near_buffer_full(void)
{
    test_io_t io;
    pad_session_t pad;
    uint32 i;
    uint8 saw_xoff = 0;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    /* Simple profile has param 5 = 1. Fill the assembly buffer with
       non-forwarding chars until the 80% high watermark trips. */
    for (i = 0; i < PAD_ASM_BUF_SIZE - 10 && !saw_xoff; i++) {
        feed_byte(&pad, 'A');
        if (pad.xoff_to_dte) saw_xoff = 1;
    }
    ASSERT_TRUE(saw_xoff);
    /* The X-OFF byte (0x13) was emitted somewhere in dte_out. */
    {
        uint8 needle = 0x13;
        uint32 j;
        int found = 0;
        for (j = 0; j < io.dte_len; j++) {
            if (io.dte_out[j] == needle) { found = 1; break; }
        }
        ASSERT_TRUE(found);
    }
}

static void test_param6_prompt_emitted_on_handshake(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* SET param 6 = 5 (prompt + signals): can't be set yet (handshake
       not complete); poke directly. */
    pad.params.values[X3_PAR_SIGNALS] = 5;
    feed(&pad, "\r");                                 /* completes handshake */
    /* §3.5.23: prompt = "*" following a format effector. Look for "*" in
       output. */
    {
        int found = 0;
        uint32 j;
        for (j = 0; j < io.dte_len; j++) {
            if (io.dte_out[j] == '*') { found = 1; break; }
        }
        ASSERT_TRUE(found);
    }
}

static void test_param6_prompt_not_emitted_when_bit_clear(void)
{
    test_io_t io;
    pad_session_t pad;
    uint32 j;
    int found = 0;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Default param 6 = 1 (signals on, prompt OFF). */
    feed(&pad, "\r");
    for (j = 0; j < io.dte_len; j++) {
        if (io.dte_out[j] == '*') { found = 1; break; }
    }
    ASSERT_TRUE(!found);
}

static void test_remote_bytes_queued_during_pad_command(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    feed_byte(&pad, 0x10);                            /* DLE escape */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    io.dte_len = 0;
    pad_input_remote(&pad, (const uint8 *)"hello", 5, 0);
    /* Bytes must NOT have reached the DTE while we're not in DATA_TRANSFER. */
    ASSERT_EQ_INT(io.dte_len, 0);
    ASSERT_EQ_INT(pad.remote_q_len, 5);
}

static void test_remote_queue_drains_on_return_to_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);                            /* recall */
    pad_input_remote(&pad, (const uint8 *)"hello", 5, 0);
    io.dte_len = 0;
    feed_byte(&pad, IA5_CR);                          /* finish empty command */
    /* Returns to DATA_TRANSFER; queued bytes drain to DTE. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_EQ_INT(pad.remote_q_len, 0);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "hello"));
}

static void test_remote_queue_drains_on_async_connect(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    /* Bridge feeds early bytes before completing the connect. */
    pad_input_remote(&pad, (const uint8 *)"hi", 2, 0);
    ASSERT_EQ_INT(pad.remote_q_len, 2);
    io.dte_len = 0;
    pad_call_connected(&pad);
    /* On completion, queue drains to DTE. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "hi"));
    ASSERT_EQ_INT(pad.remote_q_len, 0);
    x25_stub_set_async(0);
}

static void test_remote_queue_cleared_on_remote_cleared(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);
    pad_input_remote(&pad, (const uint8 *)"stale", 5, 0);
    ASSERT_EQ_INT(pad.remote_q_len, 5);
    pad_remote_cleared(&pad, PAD_CLR_REMOTE_REQUEST, 0, 0);
    /* Stale queued bytes must not survive a clear. */
    ASSERT_EQ_INT(pad.remote_q_len, 0);
}

static void test_remote_queue_overflow_drops_new(void)
{
    test_io_t io;
    pad_session_t pad;
    uint8 big[PAD_REMOTE_Q_SIZE + 100];
    memset(&io, 0, sizeof(io));
    memset(big, 'X', sizeof(big));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);                            /* leave data transfer */
    pad_input_remote(&pad, big, sizeof(big), 0);
    /* Queue is capped; overflow bytes are dropped. */
    ASSERT_EQ_INT(pad.remote_q_len, PAD_REMOTE_Q_SIZE);
}

static void test_async_selection_enters_conn_in_progress(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    /* X25_IN_PROGRESS from x25_call should put us into CONN_IN_PROGRESS
       with no ACK yet emitted. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    ASSERT_TRUE(!pad.call.connected);
    /* The echoed selection bytes were emitted, but no ACK signal yet. */
    ASSERT_TRUE(!contains(io.dte_out, io.dte_len, "\r\n"));
    x25_stub_set_async(0);
}

static void test_async_call_connected_completes_setup(void)
{
    test_io_t io;
    pad_session_t pad;
    int rc;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    io.dte_len = 0;
    rc = pad_call_connected(&pad);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
    /* §3.5.21 COM signal emitted on completion: "\r\nCOM\r\n". */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "COM"));
    x25_stub_set_async(0);
}

static void test_recall_during_conn_in_progress_enters_waiting(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    feed_byte(&pad, 0x10);                            /* DLE recall */
    /* §3.2.1.5: PAD recall during call setup escapes to WAITING_FOR_CMD. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_EQ_INT(pad.pre_recall_state, PAD_STATE_CONN_IN_PROGRESS);
    x25_stub_set_async(0);
}

static void test_empty_cmd_returns_to_conn_in_progress(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    feed_byte(&pad, 0x10);
    /* Empty command (just CR) - returns to the pre-recall state, which
       is CONN_IN_PROGRESS since the call is still being set up. */
    feed_byte(&pad, IA5_CR);
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    x25_stub_set_async(0);
}

static void test_recall_during_data_transfer_returns_to_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* Synchronous selection: into DATA_TRANSFER directly. */
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    feed_byte(&pad, 0x10);
    ASSERT_EQ_INT(pad.state, PAD_STATE_WAITING_FOR_CMD);
    ASSERT_EQ_INT(pad.pre_recall_state, PAD_STATE_DATA_TRANSFER);
    /* STAT then resume. */
    feed(&pad, "STAT\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_async_call_failure_via_remote_cleared(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    x25_stub_set_async(1);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_CONN_IN_PROGRESS);
    io.dte_len = 0;
    /* Simulate connect failure: remote/X.25 layer fires pad_remote_cleared
       with a relevant cause. */
    pad_remote_cleared(&pad, PAD_CLR_NUMBER_NOT_ASSIGNED, 0, 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!pad.call.connected);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR NP"));
    x25_stub_set_async(0);
}

static void test_pad_call_connected_noop_outside_conn_in_progress(void)
{
    test_io_t io;
    pad_session_t pad;
    int rc;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    rc = pad_call_connected(&pad);
    /* Not in CONN_IN_PROGRESS; must return non-zero and not transition. */
    ASSERT_TRUE(rc != 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_sync_selection_unchanged(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    /* Stub defaults to synchronous - existing behaviour preserved. */
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
}

static void test_remote_cleared_emits_indication(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    io.dte_len = 0;
    pad_remote_cleared(&pad, PAD_CLR_REMOTE_REQUEST, 0, 0);
    /* §3.5.17 format: "CLR <cause> C:<code> D:<diag>". DTE = remote req. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "CLR DTE"));
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!pad.call.connected);
}

static void test_remote_cleared_clears_buffers(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed(&pad, "AB");                                 /* asm_buf holds AB */
    ASSERT_EQ_INT(pad.asm_len, 2);
    pad_remote_cleared(&pad, PAD_CLR_NETWORK_PROBLEM, 0, 0);
    ASSERT_EQ_INT(pad.asm_len, 0);
    ASSERT_EQ_INT(pad.edit_len, 0);
}

static void test_remote_cleared_signals_off_no_output(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    /* Transparent profile: param 6 = 0, no service signals. */
    pad.call.connected = 1;
    pad.state = PAD_STATE_DATA_TRANSFER;
    pad_remote_cleared(&pad, PAD_CLR_REMOTE_REQUEST, 0, 0);
    /* No CLR indication emitted; state still transitions. */
    ASSERT_EQ_INT(io.dte_len, 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(!pad.call.connected);
}

static void test_remote_reset_emits_indication(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_remote_reset(&pad, PAD_RESET_REMOTE_DEVICE, 0);
    /* §3.5.7 format: "RESET <cause> D:<diag>". DTE = remote device reset. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "RESET DTE"));
    /* State stays in DATA_TRANSFER; call still up. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
    ASSERT_TRUE(pad.call.connected);
}

static void test_remote_reset_drops_assembly_buffer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    feed(&pad, "ABCD");
    ASSERT_EQ_INT(pad.asm_len, 4);
    pad_remote_reset(&pad, PAD_RESET_NETWORK_PROBLEM, 0);
    /* X.28 Table 5: data may be lost - asm buffer is dropped. */
    ASSERT_EQ_INT(pad.asm_len, 0);
}

static void test_remote_interrupted_emits_bel(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_remote_interrupted(&pad, 0);
    /* X.29-flavoured: BEL is sent to the DTE; the call stays up. */
    ASSERT_EQ_INT(io.dte_len, 1);
    ASSERT_EQ_INT(io.dte_out[0], 0x07);
    ASSERT_EQ_INT(pad.state, PAD_STATE_DATA_TRANSFER);
}

static void test_remote_interrupted_silent_when_signals_off(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");
    io.dte_len = 0;
    pad_remote_interrupted(&pad, 0);
    /* Transparent profile sets param 6 = 0 (no service signals);
       BEL is suppressed alongside the other PAD service signals. */
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_pad_set_identification_changes_banner(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_identification(&pad, "PADAWAN v1.1");
    feed(&pad, "\r");                                 /* completes handshake */
    /* X.28 §3.5.18: the configured text appears in the emitted banner. */
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "PADAWAN v1.1"));
}

static void test_pad_set_identification_empty_suppresses_banner(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    pad_set_identification(&pad, "");                 /* disable banner */
    feed(&pad, "\r");
    ASSERT_EQ_INT(io.dte_len, 0);
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
}

static void test_pad_flush_drains_assembly_buffer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "12345\r");                            /* enter data transfer */
    io.remote_len = 0;
    feed(&pad, "AB");                                 /* sit in asm buffer */
    ASSERT_EQ_INT(io.remote_len, 0);
    ASSERT_EQ_INT(pad.asm_len, 2);
    pad_flush(&pad);
    ASSERT_EQ_INT(io.remote_len, 2);
    ASSERT_MEM_EQ(io.remote_out, "AB", 2);
    ASSERT_EQ_INT(pad.asm_len, 0);
}

static void test_pad_flush_noop_outside_data_transfer(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* No call established; flush should not emit anything. */
    pad_flush(&pad);
    ASSERT_EQ_INT(io.remote_len, 0);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_handshake_starts_in_active_link(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    ASSERT_EQ_INT(pad.state, PAD_STATE_ACTIVE_LINK);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_handshake_first_byte_enters_service_request(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed_byte(&pad, 'A');
    /* Any byte transitions out of ACTIVE_LINK. With no CR yet, we are
       still in SERVICE_REQUEST. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_SERVICE_REQUEST);
    /* The byte itself is consumed by the handshake, not echoed. */
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_handshake_cr_completes_and_emits_id(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    feed(&pad, "@\r");
    /* §2.2.3-§2.2.4 + §3.5.18: PAD identification is emitted and the
       state advances to PAD_WAITING. */
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "PAD"));
}

static void test_handshake_then_command(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, &io);
    /* After the service request completes, normal commands work. */
    feed(&pad, "\rSTAT\r");
    ASSERT_TRUE(contains(io.dte_out, io.dte_len, "FREE"));
}

static void test_handshake_signals_off_suppresses_pad_id(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init_handshake(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    /* §2.2.3 NOTE: when param 6 = 0 (transparent profile) the PAD goes
       directly to PAD_WAITING without emitting the identification. */
    feed(&pad, "\r");
    ASSERT_EQ_INT(pad.state, PAD_STATE_PAD_WAITING);
    ASSERT_EQ_INT(io.dte_len, 0);
}

static void test_signals_off_suppresses_output(void)
{
    test_io_t io;
    pad_session_t pad;
    memset(&io, 0, sizeof(io));
    pad_init(&pad, X3_PROFILE_TRANSPARENT, cb_dte, cb_remote, &io);
    /* Transparent profile param 6 = 0 -> service signals suppressed. */
    feed(&pad, "STAT\r");
    /* No echo (param 2 = 0) and no service signal. */
    ASSERT_EQ_INT(io.dte_len, 0);
}

int main(void)
{
    test_init_state();
    test_stat_idle_reports_free();
    test_par_q_reads_simple_profile();
    test_set_then_par_reflects_new_value();
    test_set_invalid_value_emits_par_inv();
    test_set_partial_accept();
    test_clr_emits_confirmation();
    test_prof_loads_transparent();
    test_selection_starts_call_and_enters_data_transfer();
    test_stat_after_call_reports_engaged();
    test_data_transfer_forwards_on_cr();
    test_forwarding_alphanumeric();
    test_forwarding_esc();
    test_forwarding_disabled();
    test_echo_scrambled();
    test_echo_except_forwarding();
    test_clr_tears_down_call();
    test_auth_callback_allows_matching_nui();
    test_auth_callback_rejects_with_clr_na();
    test_auth_callback_rejects_when_no_nui();
    test_id_command_sets_session_nui();
    test_idoff_clears_session_nui();
    test_session_nui_satisfies_auth_gate();
    test_idoff_revokes_auth_for_next_call();
    test_pending_dte_replayed_on_call_connected();
    test_pending_dte_dropped_on_recall();
    test_lf_after_cr_in_dte_input();
    test_lf_not_inserted_when_bit_clear();
    test_qualified_remote_data_dropped();
    test_x29_invite_clear_tears_down_call();
    test_x29_set_updates_local_params();
    test_x29_parameter_indication_emits_par();
    test_x29_break_emits_bel_and_param8();
    test_clr_indication_includes_cause_code();
    test_id_with_no_arg_emits_nui_prompt();
    test_id_prompt_captures_next_input_as_nui();
    test_id_prompt_empty_answer_leaves_nui_unchanged();
    test_trace_callback_fires_for_dte_input();
    test_trace_callback_fires_for_remote_input();
    test_trace_callback_silent_on_zero_len();
    test_pending_dte_dropped_on_clear();
    test_clr_when_idle_returns_err();
    test_int_when_idle_returns_err();
    test_editing_cdel_removes_last_char();
    test_editing_ldel_clears_buffer();
    test_editing_ldis_redisplays_buffer();
    test_cdel_emits_backslash();
    test_ldel_emits_xxx();
    test_remote_data_forwarded_to_dte_in_data_transfer();
    test_remote_data_dropped_outside_data_transfer();
    test_idle_timer_disabled_when_param4_zero();
    test_idle_timer_flushes_after_threshold();
    test_idle_timer_resets_on_byte();
    test_idle_timer_noop_outside_data_transfer();
    test_break_bit_3_escapes_data_transfer();
    test_break_bit_4_discards_remote_output();
    test_break_param_7_zero_is_noop();
    test_cr_padding_inserts_nuls();
    test_lf_padding_only_in_data_transfer();
    test_editing_in_data_transfer_when_param15_one();
    test_editing_in_data_transfer_off_by_default();
    test_idle_timer_suspended_when_editing_in_data_transfer();
    test_echo_mask_suppresses_cr();
    test_echo_mask_suppresses_editing_chars();
    test_lf_after_cr_echo_when_param13_bit2();
    test_no_lf_after_cr_echo_when_bit2_clear();
    test_lf_insertion_in_remote_stream_when_bit0();
    test_extended_status_alias();
    test_extended_help_emits_help_signal();
    test_extended_break_does_not_escape();
    test_extended_nui_on_off_acknowledge();
    test_param12_dc3_xoffs_output();
    test_param12_dc1_clears_xoff();
    test_param12_dc1_dc3_not_echoed();
    test_param12_disabled_treats_dc1_dc3_as_data();
    test_param22_page_wait_after_n_lfs();
    test_param22_dc1_cancels_page_wait();
    test_param10_line_folding_inserts_effector();
    test_param10_cr_resets_column();
    test_param5_emits_xoff_near_buffer_full();
    test_param6_prompt_emitted_on_handshake();
    test_param6_prompt_not_emitted_when_bit_clear();
    test_remote_bytes_queued_during_pad_command();
    test_remote_queue_drains_on_return_to_data_transfer();
    test_remote_queue_drains_on_async_connect();
    test_remote_queue_cleared_on_remote_cleared();
    test_remote_queue_overflow_drops_new();
    test_async_selection_enters_conn_in_progress();
    test_async_call_connected_completes_setup();
    test_recall_during_conn_in_progress_enters_waiting();
    test_empty_cmd_returns_to_conn_in_progress();
    test_recall_during_data_transfer_returns_to_data_transfer();
    test_async_call_failure_via_remote_cleared();
    test_pad_call_connected_noop_outside_conn_in_progress();
    test_sync_selection_unchanged();
    test_remote_cleared_emits_indication();
    test_remote_cleared_clears_buffers();
    test_remote_cleared_signals_off_no_output();
    test_remote_reset_emits_indication();
    test_remote_reset_drops_assembly_buffer();
    test_remote_interrupted_emits_bel();
    test_remote_interrupted_silent_when_signals_off();
    test_pad_set_identification_changes_banner();
    test_pad_set_identification_empty_suppresses_banner();
    test_pad_flush_drains_assembly_buffer();
    test_pad_flush_noop_outside_data_transfer();
    test_handshake_starts_in_active_link();
    test_handshake_first_byte_enters_service_request();
    test_handshake_cr_completes_and_emits_id();
    test_handshake_then_command();
    test_handshake_signals_off_suppresses_pad_id();
    test_signals_off_suppresses_output();
    TEST_REPORT();
}
