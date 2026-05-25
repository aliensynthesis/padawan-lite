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
    ASSERT_TRUE(personality_by_name("tymnet") != NULL);
    ASSERT_TRUE(strcmp(personality_by_name("telenet")->name, "telenet") == 0);
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

static void test_tymnet_personality_replaces_stat(void)
{
    pad_session_t pad;
    reset_io();
    pad_init(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("tymnet"));
    pad_input_dte(&pad, (const uint8 *)"STAT\r", 5);
    ASSERT_TRUE(contains("ready"));
    ASSERT_TRUE(!contains("FREE"));
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
       in ACTIVE_LINK; the user's first CR drives complete_handshake which
       calls emit_prompt_if_enabled. For Telenet, param 6 must have the
       prompt bit (value 4) set so the '@' reaches the DTE. */
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("telenet"));
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_TRUE(contains("@"));
}

static void test_tymnet_prompt_char_visible(void)
{
    pad_session_t pad;
    reset_io();
    /* Tymnet doesn't override prompt_char so it stays '*', but the
       prompt bit MUST be on for the user to see any prompt at all. */
    pad_init_handshake(&pad, X3_PROFILE_SIMPLE, cb_dte, cb_remote, NULL);
    pad_set_personality(&pad, personality_by_name("tymnet"));
    pad_input_dte(&pad, (const uint8 *)"\r", 1);
    ASSERT_TRUE(contains("*"));
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

int main(void)
{
    test_lookup_default();
    test_lookup_known();
    test_lookup_unknown_returns_null();
    test_default_personality_is_passthrough();
    test_telenet_personality_replaces_stat();
    test_telenet_personality_replaces_err();
    test_tymnet_personality_replaces_stat();
    test_profile_overlay_applies();
    test_profile_overlay_skips_speed_param();
    test_nui_prompt_text_overridden();
    test_personality_null_reverts_to_default();
    test_telenet_prompt_char_visible();
    test_tymnet_prompt_char_visible();
    test_banner_set_by_personality();
    TEST_REPORT();
}
