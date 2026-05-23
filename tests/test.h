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

/* Minimal C89 test framework: ASSERT macros + report.
   Each test file is one translation unit with its own main(); the static
   counters below live per-TU. No external dependencies. */
#ifndef PADAWAN_TEST_H
#define PADAWAN_TEST_H

#include <stdio.h>
#include <string.h>

static int padawan_test_pass = 0;
static int padawan_test_fail = 0;

#define ASSERT_TRUE(cond) do { \
    if (cond) { \
        padawan_test_pass++; \
    } else { \
        padawan_test_fail++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define ASSERT_EQ_INT(actual, expected) do { \
    long pact = (long)(actual); \
    long pexp = (long)(expected); \
    if (pact == pexp) { \
        padawan_test_pass++; \
    } else { \
        padawan_test_fail++; \
        fprintf(stderr, "FAIL %s:%d: expected %ld, got %ld\n", \
                __FILE__, __LINE__, pexp, pact); \
    } \
} while (0)

#define ASSERT_MEM_EQ(actual, expected, len) do { \
    if (memcmp((actual), (expected), (size_t)(len)) == 0) { \
        padawan_test_pass++; \
    } else { \
        padawan_test_fail++; \
        fprintf(stderr, "FAIL %s:%d: %lu-byte buffer mismatch\n", \
                __FILE__, __LINE__, (unsigned long)(len)); \
    } \
} while (0)

#define TEST_REPORT() do { \
    int padawan_total = padawan_test_pass + padawan_test_fail; \
    if (padawan_test_fail == 0) { \
        printf("ok %d/%d\n", padawan_test_pass, padawan_total); \
        return 0; \
    } \
    printf("FAIL %d/%d failed\n", padawan_test_fail, padawan_total); \
    return 1; \
} while (0)

#endif
