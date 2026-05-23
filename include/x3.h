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

/* ITU-T X.3 (03/00) - Packet Assembly/Disassembly facility (PAD).
   Per-session PAD parameter storage, validation, and standard profiles.
   Scope for v1.0: parameters 1-30. Refs 1-22 are the classic basic set
   and are fully validated and behavioural. Refs 23-30 (extended set)
   are storage + range-checked only; behaviour is inert. They exist in
   the table so X.29 PAD-message Set/Read targeting any of them can be
   accepted and round-tripped without table-size special cases. */
#ifndef PADAWAN_X3_H
#define PADAWAN_X3_H

#include "types.h"

/* X.3 clause 3: parameter reference numbers. */
#define X3_PAR_RECALL     1   /* X.3 3.1  PAD recall using a character */
#define X3_PAR_ECHO       2   /* X.3 3.2  Echo */
#define X3_PAR_FORWARD    3   /* X.3 3.3  Selection of data forwarding character(s) */
#define X3_PAR_IDLE       4   /* X.3 3.4  Selection of idle timer delay */
#define X3_PAR_DEVICE     5   /* X.3 3.5  Ancillary device control */
#define X3_PAR_SIGNALS    6   /* X.3 3.6  Control of PAD service signals */
#define X3_PAR_BREAK      7   /* X.3 3.7  Operation on receipt of break */
#define X3_PAR_DISCARD    8   /* X.3 3.8  Discard output */
#define X3_PAR_CR_PAD     9   /* X.3 3.9  Padding after carriage return */
#define X3_PAR_FOLD       10  /* X.3 3.10 Line folding */
#define X3_PAR_SPEED      11  /* X.3 3.11 Binary speed (read-only) */
#define X3_PAR_FLOW       12  /* X.3 3.12 Flow control of PAD by start-stop DTE */
#define X3_PAR_LF_INSERT  13  /* X.3 3.13 Linefeed insertion after CR */
#define X3_PAR_LF_PAD     14  /* X.3 3.14 Linefeed padding */
#define X3_PAR_EDIT       15  /* X.3 3.15 Editing */
#define X3_PAR_CDEL       16  /* X.3 3.16 Character delete */
#define X3_PAR_LDEL       17  /* X.3 3.17 Line delete */
#define X3_PAR_LDIS       18  /* X.3 3.18 Line display */
#define X3_PAR_ESIG       19  /* X.3 3.19 Editing PAD service signals */
#define X3_PAR_MASK       20  /* X.3 3.20 Echo mask */
#define X3_PAR_PARITY     21  /* X.3 3.21 Parity treatment */
#define X3_PAR_PAGE       22  /* X.3 3.22 Page wait */
#define X3_PAR_IN_SIZE    23  /* X.3 3.23 Size of input field (extended) */
#define X3_PAR_EOF        24  /* X.3 3.24 End-of-frame char (extended) */
#define X3_PAR_EXT_FWD    25  /* X.3 3.25 Extended data forwarding signals */
#define X3_PAR_DISP_INT   26  /* X.3 3.26 Display of interrupt status */
#define X3_PAR_DISP_CONF  27  /* X.3 3.27 Display of interrupt confirm */
#define X3_PAR_DIACRITIC  28  /* X.3 3.28 Diacritic-character coding */
#define X3_PAR_EXT_ECHO   29  /* X.3 3.29 Extended echo mask */
#define X3_PAR_PKT_SIZE   30  /* X.3 3.30 Default packet size */

#define X3_PAR_MIN 1
#define X3_PAR_MAX 30

/* ITU-T standard profile identifiers, Table 3/X.28. */
#define X3_PROFILE_SIMPLE      90
#define X3_PROFILE_TRANSPARENT 91

/* Return codes. */
#define X3_OK              0
#define X3_ERR_BAD_REF     1
#define X3_ERR_BAD_VALUE   2
#define X3_ERR_READ_ONLY   3
#define X3_ERR_BAD_PROFILE 4

/* A PAD session's parameter set (X.3 clause 2.1: one set per start-stop DTE).
   Index 0 is unused so values[N] addresses parameter reference N directly. */
typedef struct {
    uint8 values[X3_PAR_MAX + 1];
} x3_params_t;

/* Load an ITU-T standard profile (X.28 Table 1) into p.
   Returns X3_OK or X3_ERR_BAD_PROFILE. */
int x3_load_profile(x3_params_t *p, uint8 profile_id);

/* Read current value (X.3 clause 2.4.2). */
int x3_get(const x3_params_t *p, uint8 id, uint8 *out_value);

/* Set current value (X.28 clauses 3.3.2, 3.5.6). */
int x3_set(x3_params_t *p, uint8 id, uint8 value);

/* Validate without storing - used by batch SET to reject the whole batch
   if any pair is invalid, before applying any. */
int x3_validate(uint8 id, uint8 value);

/* Internal setter that bypasses the read-only check for parameter 11.
   Intended for PAD initialisation / auto-baud only. */
int x3_set_speed(x3_params_t *p, uint8 speed_code);

#endif
