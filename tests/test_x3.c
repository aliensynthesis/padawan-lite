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

/* Tests for X.3 parameter store.
   Reference values cited inline from X.3 clause 3 and X.28 Table 1. */
#include "test.h"
#include "x3.h"

static void test_simple_profile_defaults(void)
{
    x3_params_t p;
    uint8 v;

    ASSERT_EQ_INT(x3_load_profile(&p, X3_PROFILE_SIMPLE), X3_OK);

    /* X.28 Table 1: Simple standard profile values. */
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_RECALL,  &v), X3_OK); ASSERT_EQ_INT(v, 1);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_ECHO,    &v), X3_OK); ASSERT_EQ_INT(v, 1);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_FORWARD, &v), X3_OK); ASSERT_EQ_INT(v, 126);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_IDLE,    &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_DEVICE,  &v), X3_OK); ASSERT_EQ_INT(v, 1);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_SIGNALS, &v), X3_OK); ASSERT_EQ_INT(v, 1);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_BREAK,   &v), X3_OK); ASSERT_EQ_INT(v, 2);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_FLOW,    &v), X3_OK); ASSERT_EQ_INT(v, 1);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_CDEL,    &v), X3_OK); ASSERT_EQ_INT(v, 127);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_LDEL,    &v), X3_OK); ASSERT_EQ_INT(v, 24);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_LDIS,    &v), X3_OK); ASSERT_EQ_INT(v, 18);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_ESIG,    &v), X3_OK); ASSERT_EQ_INT(v, 1);
}

static void test_transparent_profile_defaults(void)
{
    x3_params_t p;
    uint8 v;

    ASSERT_EQ_INT(x3_load_profile(&p, X3_PROFILE_TRANSPARENT), X3_OK);

    /* X.28 Table 1: Transparent standard profile differs from Simple in
       params 1, 2, 3, 4, 5, 6, 12. */
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_RECALL,  &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_ECHO,    &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_FORWARD, &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_IDLE,    &v), X3_OK); ASSERT_EQ_INT(v, 20);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_DEVICE,  &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_SIGNALS, &v), X3_OK); ASSERT_EQ_INT(v, 0);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_FLOW,    &v), X3_OK); ASSERT_EQ_INT(v, 0);
}

static void test_load_unknown_profile(void)
{
    x3_params_t p;
    ASSERT_EQ_INT(x3_load_profile(&p, 99), X3_ERR_BAD_PROFILE);
    ASSERT_EQ_INT(x3_load_profile(&p, 0),  X3_ERR_BAD_PROFILE);
}

static void test_get_bad_ref(void)
{
    x3_params_t p;
    uint8 v;
    x3_load_profile(&p, X3_PROFILE_SIMPLE);
    ASSERT_EQ_INT(x3_get(&p, 0, &v), X3_ERR_BAD_REF);
    /* refs 23-30 are now part of the extended set (storage + range
       check only); 31+ remain bad. */
    ASSERT_EQ_INT(x3_get(&p, 23, &v), X3_OK);
    ASSERT_EQ_INT(x3_get(&p, 30, &v), X3_OK);
    ASSERT_EQ_INT(x3_get(&p, 31, &v), X3_ERR_BAD_REF);
    ASSERT_EQ_INT(x3_get(&p, 255, &v), X3_ERR_BAD_REF);
}

static void test_set_valid_values(void)
{
    x3_params_t p;
    uint8 v;
    x3_load_profile(&p, X3_PROFILE_SIMPLE);

    /* X.3 3.1: recall = DLE (1), graphic 32-126, or 0. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 0), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 1), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 32), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 126), X3_OK);

    /* X.3 3.2: echo = 0, 1, 2, or 32-126. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_ECHO, 2), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_ECHO, 100), X3_OK);
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_ECHO, &v), X3_OK); ASSERT_EQ_INT(v, 100);

    /* X.3 3.4: idle = 0..255, all valid. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_IDLE, 255), X3_OK);

    /* X.3 3.16: cdel = 0..127 or 128/129/130 (sequences). */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_CDEL, 127), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_CDEL, 128), X3_OK);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_CDEL, 130), X3_OK);
}

static void test_set_invalid_values(void)
{
    x3_params_t p;
    x3_load_profile(&p, X3_PROFILE_SIMPLE);

    /* X.3 3.1: recall does not allow 2..31 nor 127+. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 2),   X3_ERR_BAD_VALUE);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 31),  X3_ERR_BAD_VALUE);
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_RECALL, 127), X3_ERR_BAD_VALUE);

    /* X.3 3.5: device only 0, 1, 2. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_DEVICE, 3), X3_ERR_BAD_VALUE);

    /* X.3 3.8: discard only 0 or 1. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_DISCARD, 2), X3_ERR_BAD_VALUE);

    /* X.3 3.17: ldel must be <= 127. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_LDEL, 128), X3_ERR_BAD_VALUE);
}

static void test_set_readonly(void)
{
    x3_params_t p;
    x3_load_profile(&p, X3_PROFILE_SIMPLE);
    /* X.3 3.11: parameter 11 is read-only via x3_set. */
    ASSERT_EQ_INT(x3_set(&p, X3_PAR_SPEED, 14), X3_ERR_READ_ONLY);
    /* But x3_set_speed accepts speed codes 0..19. */
    ASSERT_EQ_INT(x3_set_speed(&p, 14), X3_OK);
    ASSERT_EQ_INT(x3_set_speed(&p, 20), X3_ERR_BAD_VALUE);
}

static void test_set_failed_leaves_value_unchanged(void)
{
    x3_params_t p;
    uint8 v;
    x3_load_profile(&p, X3_PROFILE_SIMPLE);
    x3_set(&p, X3_PAR_RECALL, 200); /* invalid */
    ASSERT_EQ_INT(x3_get(&p, X3_PAR_RECALL, &v), X3_OK);
    ASSERT_EQ_INT(v, 1); /* still the Simple default */
}

int main(void)
{
    test_simple_profile_defaults();
    test_transparent_profile_defaults();
    test_load_unknown_profile();
    test_get_bad_ref();
    test_set_valid_values();
    test_set_invalid_values();
    test_set_readonly();
    test_set_failed_leaves_value_unchanged();
    TEST_REPORT();
}
