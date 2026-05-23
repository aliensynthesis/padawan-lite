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

/* X.25 layer stub interface.
   Real X.25 virtual-call support is deferred; the PAD code calls these
   entry points and the Linux platform shim logs and returns success.
   See platform/linux/x25_stub.c. */
#ifndef PADAWAN_X25_H
#define PADAWAN_X25_H

#include "types.h"

#define X25_OK                  0
#define X25_ERR_BUSY            1
#define X25_ERR_NO_ROUTE       2
#define X25_ERR_REJECTED       3
#define X25_ERR_CLEARED        4
#define X25_IN_PROGRESS        5  /* call setup not yet complete; the X.25
                                     layer will later fire
                                     pad_call_connected on success or
                                     pad_remote_cleared on failure. */
#define X25_ERR_NOT_SUPPORTED  6  /* feature unimplemented by this
                                     transport (e.g. Q-bit over Telnet). */

typedef struct {
    int32 call_id;
    int   connected;
} x25_call_t;

int x25_init(void);
int x25_call(x25_call_t *call, const char *address);
int x25_clear(x25_call_t *call, uint8 cause, uint8 diagnostic);
int x25_reset(x25_call_t *call, uint8 cause, uint8 diagnostic);
int x25_interrupt(x25_call_t *call, uint8 user_data);

/* Send len bytes on the call. qbit = 0 for normal user data, qbit = 1
   for an X.29 qualified data packet (PAD message). Transports that do
   not support qualified data MAY return X25_ERR_NOT_SUPPORTED when
   qbit != 0; the Telnet/TCP bridge does exactly that. */
int x25_send(x25_call_t *call, const uint8 *data, uint32 len, uint8 qbit);

/* Receive up to buf_size bytes. *qbit_out is set to 1 if the bytes are
   from a qualified data packet (X.29 PAD message), 0 otherwise.
   qbit_out may be NULL if the caller does not care. */
int x25_recv(x25_call_t *call, uint8 *buf, uint32 buf_size,
             uint32 *out_len, uint8 *qbit_out);

#endif
