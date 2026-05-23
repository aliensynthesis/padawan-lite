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

/* PAD session: state machine that wires X.3 parameters, X.28 command/service
   signal handling, and the X.25 call layer together.

   The session is driven by feeding bytes from the start-stop DTE
   (pad_input_dte) and from the remote (pad_input_remote). Outputs flow
   through the two callbacks supplied at init time. The X.25 layer is
   reached through include/x25.h (currently a stub - see deviations.txt). */
#ifndef PADAWAN_PAD_H
#define PADAWAN_PAD_H

#include "types.h"
#include "x3.h"
#include "x25.h"

#define PAD_EDIT_BUF_SIZE  256
#define PAD_ASM_BUF_SIZE   256
#define PAD_REMOTE_Q_SIZE  1024 /* bytes from remote held while we are not
                                   in DATA_TRANSFER, drained on entry */
#define PAD_ID_MAX         63   /* PAD identification text (X.28 §3.5.18) */
#define PAD_NUI_MAX        31   /* session-level NUI from ID command (§5.2) */
#define PAD_PENDING_SIZE   256  /* DTE bytes buffered during call setup;
                                   see deviations.txt for §3.2.1.5 note */

/* State numbers match X.28 Figures 1/X.28 and 2/X.28. State 3B (DTE
   waiting after a PAD command signal) is functionally equivalent to 3A
   per the spec and is not separately modelled. State 5A (MAP aspect
   transition) and state 8 (service signals - transient) are out of v1.1
   scope; see deviations.txt. */
typedef enum {
    PAD_STATE_ACTIVE_LINK      = 1,
    PAD_STATE_SERVICE_REQUEST  = 2,
    PAD_STATE_DTE_WAITING      = 3,
    PAD_STATE_SERVICE_READY    = 4,
    PAD_STATE_PAD_WAITING      = 5,
    PAD_STATE_PAD_COMMAND      = 6,
    PAD_STATE_CONN_IN_PROGRESS = 7,
    PAD_STATE_DATA_TRANSFER    = 9,
    PAD_STATE_WAITING_FOR_CMD  = 10
} pad_state_t;

/* Output callback. PAD invokes emit_dte when sending bytes to the DTE;
   emit_remote when assembling bytes to send into the X.25 data path. */
typedef void (*pad_emit_fn)(void *ctx, const uint8 *data, uint32 len);

/* Inbound traffic-logging direction tags. Used by pad_trace_fn. */
#define PAD_TRACE_DTE     1    /* DTE/client -> PAD (input from the user) */
#define PAD_TRACE_REMOTE  2    /* remote/service -> PAD (input from the host) */

/* Inbound traffic tap. Invoked at the top of pad_input_dte and
   pad_input_remote with the post-IAC-filter bytes those functions just
   received (i.e. what the PAD itself sees). The PAD never invokes this
   for outbound bytes (PAD -> DTE / PAD -> remote); that's by design --
   the driver hooks inbound only. NULL = no tracing (default). */
typedef void (*pad_trace_fn)(void *ctx, int direction,
                             const uint8 *data, uint32 len);

/* Authentication / authorisation callback invoked during SELECTION
   dispatch, before x25_call. Receives the parsed facility block, the
   address from the selection signal, and the session-level NUI (from
   the most recent ID command per X.28 §5.2; empty string if none).
   Return 0 to allow the call; any non-zero value rejects it (PAD emits
   "CLR NA" - access barred - per Table 6/X.28 and returns to
   PAD_WAITING). NULL = no auth check (default; all selections
   proceed). */
struct x28_facility_t;
typedef int (*pad_auth_fn)(void *ctx,
                           const struct x28_facility_t *facilities,
                           uint8 facility_count,
                           const char *address,
                           const char *session_nui);

typedef struct {
    pad_state_t state;
    x3_params_t params;
    x25_call_t  call;

    /* Editing buffer (X.28 3.6) for command entry. */
    uint8  edit_buf[PAD_EDIT_BUF_SIZE];
    uint32 edit_len;

    /* Assembly buffer for bytes from DTE in data transfer state, awaiting a
       forwarding condition (X.3 3.3 / X.3 3.4 / buffer full). */
    uint8  asm_buf[PAD_ASM_BUF_SIZE];
    uint32 asm_len;

    /* Idle timer accumulator (X.3 3.4 / param 4). Unit: twentieths of a
       second. Reset on each DTE byte and on entry to data transfer.
       Advanced by pad_tick. */
    uint32 idle_ticks;

    /* X.28 §3.2.1.5: when PAD recall transitions the session into
       WAITING_FOR_CMD, this records the state we escaped from so that
       the post-dispatch transition can return to it (DATA_TRANSFER or
       CONN_IN_PROGRESS). Unused when state isn't WAITING_FOR_CMD. */
    pad_state_t pre_recall_state;

    /* Remote-side bytes held while state != DATA_TRANSFER (typically
       during PAD-command editing after a recall, or during async call
       setup). Drained to the DTE on entry to DATA_TRANSFER. Overflow
       silently drops new bytes - see deviations.txt. */
    uint8  remote_q[PAD_REMOTE_Q_SIZE];
    uint32 remote_q_len;

    /* Flow control state (X.3 3.12 / X.28 4.14): set when DTE sends DC3,
       cleared when DTE sends DC1. While set, to_dte suppresses output. */
    int    xoff_from_dte;

    /* Flow control state (X.3 3.5): the PAD has sent X-OFF to the DTE
       because the assembly buffer is near full. Cleared after a flush. */
    int    xoff_to_dte;

    /* Page-wait state (X.3 3.22 / X.28 4.18): set after the configured
       linefeed count is reached; cleared on X-ON. While set, to_dte
       suppresses output. */
    int    page_wait_active;

    /* Line-folding column counter (X.3 3.10 / X.28 4.13). Reset on CR
       and on auto-inserted format effector. */
    uint32 fold_col;

    /* Linefeed counter for page wait (X.3 3.22). */
    uint32 lf_count;

    pad_emit_fn emit_dte;
    pad_emit_fn emit_remote;
    void       *ctx;

    /* PAD identification text (X.28 §3.5.18, network-dependent). Default
       set by pad_init; override via pad_set_identification. */
    char pad_id_text[PAD_ID_MAX + 1];

    /* Auth callback (NULL = no auth, the default). See pad_auth_fn. */
    pad_auth_fn auth_cb;
    void       *auth_ctx;

    /* Session-level NUI (X.28 §5.2). Set by ID command, cleared by
       IDOFF. Empty string when no ID is active. Passed to the auth
       callback so the gate can honour both per-call N facilities and
       session-level identity. */
    char session_nui[PAD_NUI_MAX + 1];

    /* DTE bytes typed during CONN_IN_PROGRESS, replayed through the
       data-transfer feeder on entry to DATA_TRANSFER. Padawan-Lite
       extension; X.28 §3.2.1.5 specifies these bytes be dropped.
       See deviations.txt. */
    uint8  pending_dte[PAD_PENDING_SIZE];
    uint32 pending_dte_len;

    /* Set after a bare ID command (X.28 §5.2). The next CR-terminated
       input is stored as session_nui instead of being parsed as a
       new PAD command. Cleared when the prompt is answered. */
    int    awaiting_nui;

    /* Inbound traffic tap (NULL = no trace, the default).
       See pad_trace_fn and pad_set_trace_callback. */
    pad_trace_fn trace_cb;
    void        *trace_ctx;
} pad_session_t;

/* Initialise a session: load profile_id into params, set state to PAD
   waiting, store callbacks. Returns 0 on success. */
int pad_init(pad_session_t *p, uint8 profile_id,
             pad_emit_fn dte_cb, pad_emit_fn remote_cb, void *ctx);

/* Spec-faithful initialisation (X.28 §2.2). Starts the session in
   ACTIVE_LINK. The first DTE byte transitions to SERVICE_REQUEST and
   begins consuming the service request signal; CR (0/13) completes the
   request, causing PAD identification (§3.5.18) to be emitted (when
   param 6 != 0) and the state to advance through DTE_WAITING /
   SERVICE_READY to PAD_WAITING. Bytes received during ACTIVE_LINK and
   SERVICE_REQUEST are consumed by the handshake and not passed to the
   command parser. */
int pad_init_handshake(pad_session_t *p, uint8 profile_id,
                       pad_emit_fn dte_cb, pad_emit_fn remote_cb,
                       void *ctx);

/* Feed bytes received from the DTE. Dispatches based on current state:
   command-state bytes go into the edit buffer with editing applied;
   data-transfer bytes go into the assembly buffer and forward on
   forwarding-char match. */
int pad_input_dte(pad_session_t *p, const uint8 *data, uint32 len);

/* Feed bytes received from the remote (X.25). qbit = 0 for user data
   (forwarded to DTE when in data transfer state); qbit = 1 for an
   X.29 PAD message (qualified data packet). X.29 PAD-message handling
   is not yet wired in v1.1 -- qualified data is currently dropped on
   the floor; see deviations.txt. */
int pad_input_remote(pad_session_t *p, const uint8 *data, uint32 len,
                     uint8 qbit);

/* Deliver an X.28 break signal (X.28 3.1.2 / 4.11). Actions taken are
   selected by X.3 param 7 (bitwise OR of: 1 send interrupt to remote,
   2 reset, 4 send indication-of-break, 8 escape from data transfer,
   16 discard output to DTE). */
int pad_break(pad_session_t *p);

/* Advance the idle timer (X.3 3.4) by elapsed_20ths twentieths of a
   second. When the configured idle timer (param 4) elapses in data
   transfer state with bytes pending in the assembly buffer, the buffer
   is forwarded to the remote. No-op in any other state. */
int pad_tick(pad_session_t *p, uint32 elapsed_20ths);

/* Cause codes for pad_remote_cleared, mapped to the abbreviated text in
   Table 6/X.28 ("Cause and extended dialogue mode text for clear PAD
   service signal"). */
typedef enum {
    PAD_CLR_NUMBER_BUSY              = 0,  /* OCC */
    PAD_CLR_NETWORK_PROBLEM          = 1,  /* NC  */
    PAD_CLR_INVALID_FACILITY         = 2,  /* INV */
    PAD_CLR_ACCESS_BARRED            = 3,  /* NA  */
    PAD_CLR_LOCAL_PROCEDURE_ERROR    = 4,  /* ERR */
    PAD_CLR_REMOTE_PROCEDURE_ERROR   = 5,  /* RPE */
    PAD_CLR_NUMBER_NOT_ASSIGNED      = 6,  /* NP  */
    PAD_CLR_NUMBER_OUT_OF_ORDER      = 7,  /* OOO */
    PAD_CLR_REMOTE_REQUEST           = 8,  /* DTE */
    PAD_CLR_REMOTE_DEVICE_ERROR      = 9,  /* DER */
    PAD_CLR_REVERSE_CHARGING_REFUSED = 10, /* RCH */
    PAD_CLR_INCOMPATIBLE_DESTINATION = 11, /* ID  */
    PAD_CLR_SHIP_NOT_CONTACTED       = 12, /* SHN */
    PAD_CLR_FAST_SELECT_REFUSED      = 13, /* FNA */
    PAD_CLR_CANNOT_ROUTE             = 14  /* RNA */
} pad_clear_cause_t;

/* Cause codes for pad_remote_reset, mapped to Table 5/X.28. */
typedef enum {
    PAD_RESET_REMOTE_DEVICE          = 0,  /* DTE */
    PAD_RESET_LOCAL_PROCEDURE_ERROR  = 1,  /* ERR */
    PAD_RESET_NETWORK_PROBLEM        = 2,  /* NC  */
    PAD_RESET_REMOTE_PROCEDURE_ERROR = 3   /* RPE */
} pad_reset_cause_t;

/* Called by the X.25 / bridge layer when the remote DTE clears the call,
   or when an in-progress async connect fails. Emits a clear-indication
   PAD service signal (X.28 §3.5.17) of the form
   "CLR <cause-text> C:<cause_code> D:<diagnostic>", tears down the
   session's call state, drops any in-flight buffers, and transitions
   to PAD_WAITING. Works from DATA_TRANSFER, CONN_IN_PROGRESS, or any
   state that owns a call.
   cause selects the abbreviated text (Table 6/X.28); cause_code is the
   X.25 clear-cause byte from Recommendation X.25 §5.6.6 (pass 0 if
   unknown and the abbreviated cause text alone suffices); diagnostic
   is the X.25 diagnostic byte (0 when none). */
int pad_remote_cleared(pad_session_t *p, pad_clear_cause_t cause,
                       uint8 cause_code, uint8 diagnostic);

/* Called by the X.25 / bridge layer to complete an asynchronous call
   setup that x25_call had returned X25_IN_PROGRESS for. Transitions
   CONN_IN_PROGRESS -> DATA_TRANSFER, sets call.connected, and emits the
   acknowledgement PAD service signal. No-op if not in CONN_IN_PROGRESS. */
int pad_call_connected(pad_session_t *p);

/* Called by the X.25 / bridge layer when the call is reset by the remote
   or the network. Emits a reset PAD service signal (X.28 §3.5.7), drops
   the assembly buffer ("data may be lost"), stays in DATA_TRANSFER. */
int pad_remote_reset(pad_session_t *p, pad_reset_cause_t cause,
                     uint8 diagnostic);

/* Called by the X.25 / bridge layer when the remote DTE sends an
   interrupt packet (X.29 carries the 1-byte user-data field). v1.1:
   no DTE-visible action; logged for now. See deviations.txt. */
int pad_remote_interrupted(pad_session_t *p, uint8 user_data);

/* Override the PAD identification text emitted at handshake completion
   (X.28 §3.5.18). The string is copied into the session (truncated to
   PAD_ID_MAX). Pass NULL or "" to disable the identification entirely
   (handshake still completes; just no banner is emitted). */
void pad_set_identification(pad_session_t *p, const char *text);

/* Install an auth callback for this session. Invoked during SELECTION
   dispatch before any X.25 call is placed. Pass NULL to remove. */
void pad_set_auth_callback(pad_session_t *p, pad_auth_fn cb, void *ctx);

/* Install an inbound traffic tap for this session. Pass NULL to remove. */
void pad_set_trace_callback(pad_session_t *p, pad_trace_fn cb, void *ctx);

/* Forward any bytes currently held in the assembly buffer to the remote.
   Intended for clean shutdown in non-interactive drivers that would
   otherwise drop pending data when stdin EOF arrives. No-op outside
   DATA_TRANSFER state. */
int pad_flush(pad_session_t *p);

#endif
