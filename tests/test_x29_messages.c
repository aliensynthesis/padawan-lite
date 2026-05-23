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

/* Tests for the X.29 PAD-message encode/decode layer. */
#include "test.h"
#include "x29.h"
#include <string.h>

static void test_decode_invite_clear(void)
{
    x29_message_t m;
    uint8 buf[] = { 0x01 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_INVITE_CLEAR);
    ASSERT_EQ_INT(m.pair_count, 0);
}

static void test_decode_set(void)
{
    x29_message_t m;
    /* §4.5.1: Set, two pairs: (2,0) and (3,126). */
    uint8 buf[] = { 0x02, 2, 0, 3, 126 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_SET);
    ASSERT_EQ_INT(m.pair_count, 2);
    ASSERT_EQ_INT(m.pairs[0].ref, 2);
    ASSERT_EQ_INT(m.pairs[0].value, 0);
    ASSERT_EQ_INT(m.pairs[1].ref, 3);
    ASSERT_EQ_INT(m.pairs[1].value, 126);
}

static void test_decode_read(void)
{
    x29_message_t m;
    /* §4.5.2: Read three refs (2, 3, 4). */
    uint8 buf[] = { 0x04, 2, 3, 4 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_READ);
    ASSERT_EQ_INT(m.pair_count, 3);
    ASSERT_EQ_INT(m.pairs[0].ref, 2);
    ASSERT_EQ_INT(m.pairs[1].ref, 3);
    ASSERT_EQ_INT(m.pairs[2].ref, 4);
}

static void test_decode_parameter_indication(void)
{
    x29_message_t m;
    /* §4.5.4: PI carrying current values for refs 2 and 4. */
    uint8 buf[] = { 0x00, 2, 1, 4, 20 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_PARAMETER_IND);
    ASSERT_EQ_INT(m.pair_count, 2);
    ASSERT_EQ_INT(m.pairs[0].value, 1);
    ASSERT_EQ_INT(m.pairs[1].value, 20);
}

static void test_decode_set_and_read(void)
{
    x29_message_t m;
    uint8 buf[] = { 0x06, 2, 0 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_SET_READ);
    ASSERT_EQ_INT(m.pair_count, 1);
}

static void test_decode_break_empty(void)
{
    x29_message_t m;
    uint8 buf[] = { 0x03 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_BREAK);
    ASSERT_EQ_INT(m.break_has_param8, 0);
}

static void test_decode_break_with_param8(void)
{
    x29_message_t m;
    /* Pair form: {8, 1} = set param 8 (discard output) to 1. */
    uint8 buf[] = { 0x03, 8, 1 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_BREAK);
    ASSERT_EQ_INT(m.break_has_param8, 1);
    ASSERT_EQ_INT(m.break_param8, 1);
}

static void test_decode_error(void)
{
    x29_message_t m;
    /* Error: bad parameter (0x01), offending msg type was Set (0x02). */
    uint8 buf[] = { 0x05, 0x01, 0x02 };
    ASSERT_EQ_INT(x29_decode(buf, sizeof(buf), &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_ERROR);
    ASSERT_EQ_INT(m.error_code, X29_ERR_BAD_PARAMETER);
    ASSERT_EQ_INT(m.error_msg_type, X29_MSG_SET);
}

static void test_decode_rejects_odd_set_body(void)
{
    x29_message_t m;
    /* §4.5.1: Set body must be even-length (ref + value pairs). */
    uint8 buf[] = { 0x02, 2, 0, 3 };          /* trailing odd byte */
    ASSERT_TRUE(x29_decode(buf, sizeof(buf), &m) < 0);
}

static void test_decode_rejects_empty_input(void)
{
    x29_message_t m;
    uint8 dummy = 0;
    ASSERT_TRUE(x29_decode(&dummy, 0, &m) < 0);
}

static void test_decode_unknown_type(void)
{
    x29_message_t m;
    uint8 buf[] = { 0x42 };
    ASSERT_TRUE(x29_decode(buf, sizeof(buf), &m) < 0);
    ASSERT_EQ_INT(m.type, X29_MSG_UNKNOWN);
}

static void test_encode_invite_clear(void)
{
    uint8 buf[4];
    int32 n = x29_encode_invite_clear(buf, sizeof(buf));
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(buf[0], 0x01);
}

static void test_encode_set(void)
{
    uint8 buf[16];
    x29_pair_t pairs[2];
    int32 n;
    pairs[0].ref = 2; pairs[0].value = 0;
    pairs[1].ref = 3; pairs[1].value = 126;
    n = x29_encode_set(pairs, 2, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 5);
    ASSERT_EQ_INT(buf[0], 0x02);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(buf[2], 0);
    ASSERT_EQ_INT(buf[3], 3);
    ASSERT_EQ_INT(buf[4], 126);
}

static void test_encode_read(void)
{
    uint8 buf[8];
    uint8 refs[3] = { 2, 3, 4 };
    int32 n = x29_encode_read(refs, 3, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 4);
    ASSERT_EQ_INT(buf[0], 0x04);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(buf[2], 3);
    ASSERT_EQ_INT(buf[3], 4);
}

static void test_encode_parameter_indication(void)
{
    uint8 buf[8];
    x29_pair_t pairs[1];
    int32 n;
    pairs[0].ref = 2; pairs[0].value = 1;
    n = x29_encode_parameter_ind(pairs, 1, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_INT(buf[0], 0x00);
    ASSERT_EQ_INT(buf[1], 2);
    ASSERT_EQ_INT(buf[2], 1);
}

static void test_encode_break_empty(void)
{
    uint8 buf[4];
    int32 n = x29_encode_break(0, 0, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 1);
    ASSERT_EQ_INT(buf[0], 0x03);
}

static void test_encode_break_with_param8(void)
{
    uint8 buf[4];
    int32 n = x29_encode_break(1, 1, buf, sizeof(buf));
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_INT(buf[0], 0x03);
    ASSERT_EQ_INT(buf[1], 8);
    ASSERT_EQ_INT(buf[2], 1);
}

static void test_encode_error(void)
{
    uint8 buf[4];
    int32 n = x29_encode_error(X29_ERR_BAD_VALUE, X29_MSG_SET,
                               buf, sizeof(buf));
    ASSERT_EQ_INT(n, 3);
    ASSERT_EQ_INT(buf[0], 0x05);
    ASSERT_EQ_INT(buf[1], X29_ERR_BAD_VALUE);
    ASSERT_EQ_INT(buf[2], X29_MSG_SET);
}

static void test_encode_overflow_returns_negative(void)
{
    uint8 buf[1];
    x29_pair_t pairs[2];
    pairs[0].ref = 2; pairs[0].value = 0;
    pairs[1].ref = 3; pairs[1].value = 0;
    ASSERT_TRUE(x29_encode_set(pairs, 2, buf, sizeof(buf)) < 0);
}

static void test_encode_decode_roundtrip(void)
{
    uint8 buf[32];
    x29_pair_t pairs[3];
    x29_message_t m;
    int32 n;
    pairs[0].ref = 2;  pairs[0].value = 0;
    pairs[1].ref = 3;  pairs[1].value = 126;
    pairs[2].ref = 7;  pairs[2].value = 2;
    n = x29_encode_set(pairs, 3, buf, sizeof(buf));
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_INT(x29_decode(buf, (uint32)n, &m), 0);
    ASSERT_EQ_INT(m.type, X29_MSG_SET);
    ASSERT_EQ_INT(m.pair_count, 3);
    ASSERT_EQ_INT(m.pairs[2].ref, 7);
    ASSERT_EQ_INT(m.pairs[2].value, 2);
}

int main(void)
{
    test_decode_invite_clear();
    test_decode_set();
    test_decode_read();
    test_decode_parameter_indication();
    test_decode_set_and_read();
    test_decode_break_empty();
    test_decode_break_with_param8();
    test_decode_error();
    test_decode_rejects_odd_set_body();
    test_decode_rejects_empty_input();
    test_decode_unknown_type();
    test_encode_invite_clear();
    test_encode_set();
    test_encode_read();
    test_encode_parameter_indication();
    test_encode_break_empty();
    test_encode_break_with_param8();
    test_encode_error();
    test_encode_overflow_returns_negative();
    test_encode_decode_roundtrip();
    TEST_REPORT();
}
