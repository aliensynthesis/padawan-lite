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

/* ITU-T X.3 parameter store implementation.
   Validation rules come from X.3 (03/00) clause 3; profile values from
   X.28 (12/97) Table 1. */
#include "x3.h"

/* X.28 Table 1: Simple standard profile (id 90). Index 0 unused. */
static const uint8 PROFILE_SIMPLE[X3_PAR_MAX + 1] = {
    0,
    1,    /*  1 recall:    DLE */
    1,    /*  2 echo:      on */
    126,  /*  3 forward:   cols 0,1 of IA5 + DEL */
    0,    /*  4 idle:      no timeout */
    1,    /*  5 device:    X-ON/X-OFF in data transfer */
    1,    /*  6 signals:   standard format */
    2,    /*  7 break:     reset */
    0,    /*  8 discard:   normal delivery */
    0,    /*  9 cr_pad:    none */
    0,    /* 10 fold:      none */
    14,   /* 11 speed:     9600 bit/s.  VERIFY: Table 1 reads "indicate speed
                              of DTE" without specifying a value; 14 chosen as
                              a sensible async-terminal default. */
    1,    /* 12 flow:      X-ON/X-OFF */
    0,    /* 13 lf_insert: none */
    0,    /* 14 lf_pad:    none */
    0,    /* 15 edit:      off in data transfer */
    127,  /* 16 cdel:      DEL */
    24,   /* 17 ldel:      CAN */
    18,   /* 18 ldis:      DC2 */
    1,    /* 19 esig:      printing-terminal editing signals */
    0,    /* 20 mask:      echo all */
    0,    /* 21 parity:    none */
    0,    /* 22 page:      disabled */
    0,    /* 23 in_size:   no limit (extended set, inert) */
    0,    /* 24 eof:       none */
    0,    /* 25 ext_fwd:   no extended forwarding signals */
    0,    /* 26 disp_int:  no interrupt display */
    0,    /* 27 disp_conf: no interrupt-confirm display */
    0,    /* 28 diacritic: no diacritic coding */
    0,    /* 29 ext_echo:  no extended echo mask */
    0     /* 30 pkt_size:  default packet size (network-dependent) */
};

/* X.28 Table 1: Transparent standard profile (id 91). */
static const uint8 PROFILE_TRANSPARENT[X3_PAR_MAX + 1] = {
    0,
    0,    /*  1 recall:    not possible */
    0,    /*  2 echo:      off */
    0,    /*  3 forward:   no forwarding */
    20,   /*  4 idle:      1 second (20 * 1/20 s) */
    0,    /*  5 device:    no X-ON/X-OFF */
    0,    /*  6 signals:   no service signals */
    2,    /*  7 break:     reset */
    0,    /*  8 discard:   normal delivery */
    0,    /*  9 cr_pad:    none */
    0,    /* 10 fold:      none */
    14,   /* 11 speed:     9600 bit/s (see VERIFY in SIMPLE profile). */
    0,    /* 12 flow:      no X-ON/X-OFF */
    0,    /* 13 lf_insert: none */
    0,    /* 14 lf_pad:    none */
    0,    /* 15 edit:      off */
    127,  /* 16 cdel:      DEL */
    24,   /* 17 ldel:      CAN */
    18,   /* 18 ldis:      DC2 */
    1,    /* 19 esig:      printing-terminal editing signals */
    0,    /* 20 mask:      echo all */
    0,    /* 21 parity:    none */
    0,    /* 22 page:      disabled */
    0,    /* 23 in_size:   no limit (extended set, inert) */
    0,    /* 24 eof:       none */
    0,    /* 25 ext_fwd:   no extended forwarding signals */
    0,    /* 26 disp_int:  no interrupt display */
    0,    /* 27 disp_conf: no interrupt-confirm display */
    0,    /* 28 diacritic: no diacritic coding */
    0,    /* 29 ext_echo:  no extended echo mask */
    0     /* 30 pkt_size:  default packet size */
};

int x3_load_profile(x3_params_t *p, uint8 profile_id)
{
    const uint8 *src;
    uint8 i;

    if (profile_id == X3_PROFILE_SIMPLE) {
        src = PROFILE_SIMPLE;
    } else if (profile_id == X3_PROFILE_TRANSPARENT) {
        src = PROFILE_TRANSPARENT;
    } else {
        return X3_ERR_BAD_PROFILE;
    }

    for (i = 0; i <= X3_PAR_MAX; i++) {
        p->values[i] = src[i];
    }
    return X3_OK;
}

int x3_get(const x3_params_t *p, uint8 id, uint8 *out_value)
{
    if (id < X3_PAR_MIN || id > X3_PAR_MAX) {
        return X3_ERR_BAD_REF;
    }
    *out_value = p->values[id];
    return X3_OK;
}

/* X.3 clause 3: legal value sets per parameter.
   Bitmask parameters accept any combination of their basic values, so the
   check is a range cap on the OR of the documented bits. */
int x3_validate(uint8 id, uint8 value)
{
    switch (id) {
    case X3_PAR_RECALL:
        if (value == 0 || value == 1) return X3_OK;
        if (value >= 32 && value <= 126) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_ECHO:
        if (value <= 2) return X3_OK;
        if (value >= 32 && value <= 126) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_FORWARD:
        /* X.3 3.3: bitmask of 1,2,4,8,16,32,64 = max OR is 127. */
        if (value <= 127) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_IDLE:
        return X3_OK; /* 0-255 all valid */

    case X3_PAR_DEVICE:
        if (value <= 2) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_SIGNALS:
        /* X.3 3.6: low nibble 0,1,2,4,5 plus high nibble for extended dialogue
           mode (16,32,48,64) and network-dependent 8-15. v1.2 accepts the full
           byte; a tighter check would reject impossible combinations like
           value 7. VERIFY: spec leaves "network-dependent" range open. */
        return X3_OK;

    case X3_PAR_BREAK:
        /* X.3 3.7: bitmask 1,2,4,8,16. Max OR is 31. */
        if (value <= 31) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_DISCARD:
        if (value <= 1) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_CR_PAD:
        return X3_OK;

    case X3_PAR_FOLD:
        return X3_OK;

    case X3_PAR_SPEED:
        return X3_ERR_READ_ONLY;

    case X3_PAR_FLOW:
        if (value <= 1) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_LF_INSERT:
        /* X.3 3.13: bitmask 1,2,4. Max OR is 7. */
        if (value <= 7) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_LF_PAD:
        return X3_OK;

    case X3_PAR_EDIT:
        if (value <= 1) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_CDEL:
        if (value <= 127) return X3_OK;
        if (value >= 128 && value <= 130) return X3_OK; /* 1/3 4/7, 1/3 1/3, 2/10 2/10 */
        return X3_ERR_BAD_VALUE;

    case X3_PAR_LDEL:
        if (value <= 127) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_LDIS:
        if (value <= 127) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_ESIG:
        if (value <= 2) return X3_OK;
        if (value == 8) return X3_OK;
        if (value >= 32 && value <= 126) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_MASK:
        /* X.3 3.20: bitmask 1,2,4,8,16,32,64,128. Full byte range valid. */
        return X3_OK;

    case X3_PAR_PARITY:
        /* X.3 3.21: 0,1,2,4. X.28 2.1.2 also defines combined value 3 = check
           and generate parity. Accept 0..4. */
        if (value <= 4) return X3_OK;
        return X3_ERR_BAD_VALUE;

    case X3_PAR_PAGE:
        return X3_OK;

    /* X.3 extended parameter set (refs 23-30). Storage + range checks
       only; behaviour is inert in v1.2. Documented in deviations.txt. */
    case X3_PAR_IN_SIZE:    /* 0-255: input field size (0 = unlimited) */
        return X3_OK;
    case X3_PAR_EOF:        /* IA5 char (0-127), or 0 = none */
        if (value <= 127) return X3_OK;
        return X3_ERR_BAD_VALUE;
    case X3_PAR_EXT_FWD:    /* X.3 3.25: bitmask 1..63 */
        if (value <= 63) return X3_OK;
        return X3_ERR_BAD_VALUE;
    case X3_PAR_DISP_INT:
    case X3_PAR_DISP_CONF:
    case X3_PAR_DIACRITIC:
        if (value <= 1) return X3_OK;
        return X3_ERR_BAD_VALUE;
    case X3_PAR_EXT_ECHO:   /* X.3 3.29: bitmask, full byte valid */
        return X3_OK;
    case X3_PAR_PKT_SIZE:   /* X.25 packet size code: 4..12 typical */
        if (value <= 12) return X3_OK;
        return X3_ERR_BAD_VALUE;

    default:
        return X3_ERR_BAD_REF;
    }
}

int x3_set(x3_params_t *p, uint8 id, uint8 value)
{
    int rc = x3_validate(id, value);
    if (rc != X3_OK) {
        return rc;
    }
    p->values[id] = value;
    return X3_OK;
}

int x3_set_speed(x3_params_t *p, uint8 speed_code)
{
    /* X.3 3.11 enumerates speed codes 0..19. */
    if (speed_code > 19) {
        return X3_ERR_BAD_VALUE;
    }
    p->values[X3_PAR_SPEED] = speed_code;
    return X3_OK;
}
