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

/* X.25 stub for Linux: logs to stderr, performs no real network I/O.
   Replace with a real X.25 implementation when the lower layer lands. */
#include "x25.h"
#include "x25_stub.h"
#include "platform.h"
#include <stdio.h>

static int g_async_mode = 0;

void x25_stub_set_async(int async)
{
    g_async_mode = async ? 1 : 0;
}

int x25_init(void)
{
    fprintf(stderr, "[x25 stub] init\n");
    return X25_OK;
}

int x25_call(x25_call_t *call, const char *address)
{
    fprintf(stderr, "[x25 stub] call \"%s\"%s\n", address,
            g_async_mode ? " (async)" : "");
    call->call_id = 1;
    if (g_async_mode) {
        call->connected = 0;
        return X25_IN_PROGRESS;
    }
    call->connected = 1;
    return X25_OK;
}

int x25_clear(x25_call_t *call, uint8 cause, uint8 diagnostic)
{
    fprintf(stderr, "[x25 stub] clear cause=%u diag=%u\n",
            (unsigned)cause, (unsigned)diagnostic);
    call->connected = 0;
    return X25_OK;
}

int x25_reset(x25_call_t *call, uint8 cause, uint8 diagnostic)
{
    fprintf(stderr, "[x25 stub] reset cause=%u diag=%u\n",
            (unsigned)cause, (unsigned)diagnostic);
    (void)call;
    return X25_OK;
}

int x25_interrupt(x25_call_t *call, uint8 user_data)
{
    fprintf(stderr, "[x25 stub] interrupt 0x%02x\n", (unsigned)user_data);
    (void)call;
    return X25_OK;
}

int x25_send(x25_call_t *call, const uint8 *data, uint32 len, uint8 qbit)
{
    fprintf(stderr, "[x25 stub] send%s %lu bytes\n",
            qbit ? " (Q)" : "", (unsigned long)len);
    (void)call;
    (void)data;
    return X25_OK;
}

int x25_recv(x25_call_t *call, uint8 *buf, uint32 buf_size,
             uint32 *out_len, uint8 *qbit_out)
{
    (void)call;
    (void)buf;
    (void)buf_size;
    *out_len = 0;
    if (qbit_out != NULL) *qbit_out = 0;
    return X25_OK;
}
