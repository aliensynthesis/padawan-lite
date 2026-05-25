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

/* PAD personality registry.

   VERIFY: The Telenet and Tymnet personality data below is a
   best-effort reconstruction from general historical knowledge of
   1970s/80s PSPDN PAD behaviour. Specific strings ("BUSY",
   "PLEASE LOG IN:", "NETWORK CONGESTION", etc.) have not been
   verified against primary-source operator manuals; they were
   populated to give the personality system a useful initial shape
   for exhibit-style work. Anyone targeting bit-for-bit Telenet or
   Tymnet fidelity should consult the original GTE Telenet /
   Tymshare manuals (see bitsavers.org and archive.org) and submit
   corrections. The X.28-standard "default" personality below is
   accurate per ITU-T X.28 (12/97). */

#include "personality.h"
#include "pad.h"

#include <string.h>

/* ------------------------------------------------------------------------- */
/* default personality (X.28 standard; all overrides NULL)                   */
/* ------------------------------------------------------------------------- */

static const personality_t PERSONALITY_DEFAULT = {
    "default",
    NULL,           /* banner: keep whatever pad_set_identification gave */
    0,              /* prompt_char: 0 => X.28 default '*' */
    NULL,           /* nui_prompt: default "NUI?" */
    NULL,           /* connected_text: default "COM" */
    NULL,           /* free_text */
    NULL,           /* engaged_text */
    NULL,           /* err_text */
    {               /* clr_text[]: all NULL => X.28 Table 6 abbreviations */
        NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL,
        NULL, NULL, NULL, NULL, NULL
    },
    NULL            /* profile_overlay: none */
};

/* ------------------------------------------------------------------------- */
/* telenet personality (best-effort; see VERIFY note at top of file)         */
/* ------------------------------------------------------------------------- */

/* VERIFY: '@' as prompt char is the commonly-cited Telenet convention.
   The banner format historically included a node identifier
   ("TELENET 215 5D" or similar); without authoritative primary
   sources we emit just "TELENET" here and let the user override
   with pad_set_identification for a specific node ID. */
static const char *const TELENET_CLR_TEXT[PERSONALITY_CLR_CAUSE_COUNT] = {
    "BUSY",                     /* 0  OCC -> Telenet: VERIFY */
    "NETWORK CONGESTION",       /* 1  NC  -> Telenet: VERIFY */
    "INVALID FACILITY",         /* 2  INV -> Telenet: VERIFY */
    "ACCESS BARRED",            /* 3  NA  -> Telenet: VERIFY */
    "LOCAL ERROR",              /* 4  ERR -> Telenet: VERIFY */
    "REMOTE ERROR",             /* 5  RPE -> Telenet: VERIFY */
    "NOT REACHABLE",            /* 6  NP  -> Telenet: VERIFY */
    "OUT OF ORDER",             /* 7  OOO -> Telenet: VERIFY */
    "DISCONNECTED",             /* 8  DTE -> Telenet: VERIFY */
    "REMOTE DEVICE ERROR",      /* 9  DER -> Telenet: VERIFY */
    "COLLECT REFUSED",          /* 10 RCH -> Telenet: VERIFY */
    "INCOMPATIBLE DESTINATION", /* 11 ID  -> Telenet: VERIFY */
    "SHIP NOT CONTACTED",       /* 12 SHN -> Telenet: VERIFY */
    "FAST SELECT REFUSED",      /* 13 FNA -> Telenet: VERIFY */
    "CANNOT ROUTE"              /* 14 RNA -> Telenet: VERIFY */
};

/* Telenet's PAD parameter conventions favoured echo-on with CR
   forwarding and a moderate idle timer. PERSONALITY_KEEP leaves the
   profile default in place. VERIFY against original GTE doc. */
static const uint8 TELENET_PROFILE_OVERLAY[X3_PAR_MAX + 1] = {
    0,                            /* 0  unused */
    PERSONALITY_KEEP,             /* 1  recall */
    1,                            /* 2  echo on */
    2,                            /* 3  forward on CR only (VERIFY) */
    PERSONALITY_KEEP,             /* 4  idle */
    1,                            /* 5  device (X-ON/X-OFF) */
    5,                            /* 6  signals: standard (1) + prompt bit (4) */
    2,                            /* 7  break -> reset */
    PERSONALITY_KEEP,             /* 8  discard */
    PERSONALITY_KEEP,             /* 9  cr_pad */
    PERSONALITY_KEEP,             /* 10 fold */
    PERSONALITY_KEEP,             /* 11 speed (read-only) */
    1,                            /* 12 flow */
    PERSONALITY_KEEP,             /* 13 lf_insert */
    PERSONALITY_KEEP,             /* 14 lf_pad */
    PERSONALITY_KEEP,             /* 15 edit */
    PERSONALITY_KEEP,             /* 16 cdel */
    PERSONALITY_KEEP,             /* 17 ldel */
    PERSONALITY_KEEP,             /* 18 ldis */
    PERSONALITY_KEEP,             /* 19 esig */
    PERSONALITY_KEEP,             /* 20 mask */
    PERSONALITY_KEEP,             /* 21 parity */
    PERSONALITY_KEEP,             /* 22 page */
    PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP,
    PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP
};

static const personality_t PERSONALITY_TELENET = {
    "telenet",
    "TELENET",                  /* banner: minimal; real node-id varies */
    '@',                        /* prompt: VERIFY */
    "ID?",                      /* NUI prompt: VERIFY */
    "CONNECTED",                /* COM -> "CONNECTED" (VERIFY) */
    "READY",                    /* FREE -> "READY" (VERIFY) */
    "BUSY",                     /* ENGAGED -> "BUSY" (VERIFY) */
    "?",                        /* ERR -> "?" (VERIFY; common 80s convention) */
    {
        TELENET_CLR_TEXT[0],  TELENET_CLR_TEXT[1],  TELENET_CLR_TEXT[2],
        TELENET_CLR_TEXT[3],  TELENET_CLR_TEXT[4],  TELENET_CLR_TEXT[5],
        TELENET_CLR_TEXT[6],  TELENET_CLR_TEXT[7],  TELENET_CLR_TEXT[8],
        TELENET_CLR_TEXT[9],  TELENET_CLR_TEXT[10], TELENET_CLR_TEXT[11],
        TELENET_CLR_TEXT[12], TELENET_CLR_TEXT[13], TELENET_CLR_TEXT[14]
    },
    TELENET_PROFILE_OVERLAY
};

/* ------------------------------------------------------------------------- */
/* tymnet personality (best-effort; see VERIFY note at top of file)          */
/* ------------------------------------------------------------------------- */

/* VERIFY: Tymnet's PAD used a "please log in:" prompt at session
   start and was reached by typing a particular trigger character
   ('a' or 'A') so the network could autodetect line speed and
   parity. Tymnet's signal text was more verbose than Telenet's. */
static const char *const TYMNET_CLR_TEXT[PERSONALITY_CLR_CAUSE_COUNT] = {
    "host busy",                       /* 0  OCC  VERIFY */
    "network congested",               /* 1  NC   VERIFY */
    "invalid request",                 /* 2  INV  VERIFY */
    "access denied",                   /* 3  NA   VERIFY */
    "local error",                     /* 4  ERR  VERIFY */
    "remote error",                    /* 5  RPE  VERIFY */
    "host not available",              /* 6  NP   VERIFY */
    "host down",                       /* 7  OOO  VERIFY */
    "disconnected by host",            /* 8  DTE  VERIFY */
    "host device error",               /* 9  DER  VERIFY */
    "reverse charging refused",        /* 10 RCH  VERIFY */
    "incompatible destination",        /* 11 ID   VERIFY */
    "ship not contacted",              /* 12 SHN  VERIFY */
    "fast select refused",             /* 13 FNA  VERIFY */
    "cannot route call"                /* 14 RNA  VERIFY */
};

/* Tymnet's PAD defaults favoured line-mode editing. VERIFY against
   original Tymshare documentation. */
static const uint8 TYMNET_PROFILE_OVERLAY[X3_PAR_MAX + 1] = {
    0,                            /* 0  unused */
    PERSONALITY_KEEP,             /* 1  recall */
    1,                            /* 2  echo on */
    2,                            /* 3  forward on CR (VERIFY) */
    PERSONALITY_KEEP,             /* 4  idle */
    1,                            /* 5  device */
    5,                            /* 6  signals: standard (1) + prompt bit (4) */
    2,                            /* 7  break */
    PERSONALITY_KEEP,             /* 8  discard */
    PERSONALITY_KEEP,             /* 9  cr_pad */
    PERSONALITY_KEEP,             /* 10 fold */
    PERSONALITY_KEEP,             /* 11 speed */
    1,                            /* 12 flow */
    PERSONALITY_KEEP,             /* 13 lf_insert */
    PERSONALITY_KEEP,             /* 14 lf_pad */
    1,                            /* 15 edit ON (Tymnet line-mode VERIFY) */
    PERSONALITY_KEEP,             /* 16 cdel */
    PERSONALITY_KEEP,             /* 17 ldel */
    PERSONALITY_KEEP,             /* 18 ldis */
    PERSONALITY_KEEP,             /* 19 esig */
    PERSONALITY_KEEP,             /* 20 mask */
    PERSONALITY_KEEP,             /* 21 parity */
    PERSONALITY_KEEP,             /* 22 page */
    PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP,
    PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP, PERSONALITY_KEEP
};

static const personality_t PERSONALITY_TYMNET = {
    "tymnet",
    "please log in:",                  /* VERIFY */
    0,                                 /* prompt_char: keep '*' (VERIFY) */
    "user name:",                      /* NUI prompt: VERIFY */
    "host connected",                  /* COM (VERIFY) */
    "ready",                           /* FREE (VERIFY) */
    "in session",                      /* ENGAGED (VERIFY) */
    "command error",                   /* ERR (VERIFY) */
    {
        TYMNET_CLR_TEXT[0],  TYMNET_CLR_TEXT[1],  TYMNET_CLR_TEXT[2],
        TYMNET_CLR_TEXT[3],  TYMNET_CLR_TEXT[4],  TYMNET_CLR_TEXT[5],
        TYMNET_CLR_TEXT[6],  TYMNET_CLR_TEXT[7],  TYMNET_CLR_TEXT[8],
        TYMNET_CLR_TEXT[9],  TYMNET_CLR_TEXT[10], TYMNET_CLR_TEXT[11],
        TYMNET_CLR_TEXT[12], TYMNET_CLR_TEXT[13], TYMNET_CLR_TEXT[14]
    },
    TYMNET_PROFILE_OVERLAY
};

/* ------------------------------------------------------------------------- */
/* registry + lookup                                                         */
/* ------------------------------------------------------------------------- */

static const personality_t *const REGISTRY[] = {
    &PERSONALITY_DEFAULT,
    &PERSONALITY_TELENET,
    &PERSONALITY_TYMNET,
    NULL
};

const personality_t *personality_by_name(const char *name)
{
    uint32 i;
    if (name == NULL) return &PERSONALITY_DEFAULT;
    for (i = 0; REGISTRY[i] != NULL; i++) {
        if (strcmp(REGISTRY[i]->name, name) == 0) return REGISTRY[i];
    }
    return NULL;
}

void personality_apply_profile_overlay(const personality_t *pers,
                                       x3_params_t *params)
{
    uint8 i;
    if (pers == NULL || pers->profile_overlay == NULL || params == NULL) {
        return;
    }
    for (i = X3_PAR_MIN; i <= X3_PAR_MAX; i++) {
        uint8 v = pers->profile_overlay[i];
        if (v == PERSONALITY_KEEP) continue;
        if (i == X3_PAR_SPEED) continue;   /* read-only */
        if (x3_validate(i, v) == X3_OK) {
            params->values[i] = v;
        }
    }
}
