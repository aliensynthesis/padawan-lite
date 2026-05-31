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

/*
 * Telos - a small, spec-rigid Telnet protocol engine.
 *
 * Scope: RFC 854 (NVT + commands), RFC 855 (option negotiation
 * framework), RFC 856 (BINARY), RFC 857 (ECHO), RFC 858 (SGA),
 * RFC 1073 (NAWS), RFC 1091 (TERMINAL-TYPE), RFC 1143 (Q-method
 * loop avoidance).
 *
 * Not in scope: sockets, threads, I/O multiplexing, ANSI escape
 * sequences (DA1 / VT52 Identify and friends), terminal driver
 * semantics. Telos is a pure state machine; the caller owns the
 * transport and feeds bytes in and writes bytes out via callbacks.
 *
 * Design principles:
 *   - Caller-allocated session struct; no malloc.
 *   - C89, project-defined integer types (uint8/uint16/uint32 from
 *     types.h), block-comment style throughout.
 *   - Strict in what we send; tolerant in what we accept (Postel's law).
 *     Optional STRICT_PEER flag turns peer violations into events.
 *   - All 256 option codes tracked via Q-method (RFC 1143).
 *   - Caller drives I/O via three callbacks: policy, event, write.
 */

#ifndef PADAWAN_TELOS_H
#define PADAWAN_TELOS_H

#include "types.h"

/* === Constants ====================================================== */

/* RFC 854 single-byte command codes (preceded by IAC=0xFF on the wire). */
#define TELOS_CMD_SE      240   /* End of subnegotiation parameters */
#define TELOS_CMD_NOP     241   /* No operation */
#define TELOS_CMD_DM      242   /* Data Mark — synch marker */
#define TELOS_CMD_BRK     243   /* Break */
#define TELOS_CMD_IP      244   /* Interrupt Process */
#define TELOS_CMD_AO      245   /* Abort Output */
#define TELOS_CMD_AYT     246   /* Are You There */
#define TELOS_CMD_EC      247   /* Erase Character */
#define TELOS_CMD_EL      248   /* Erase Line */
#define TELOS_CMD_GA      249   /* Go Ahead (paired with SGA negotiation) */
#define TELOS_CMD_SB      250   /* Begin subnegotiation */
#define TELOS_CMD_WILL    251   /* Negotiation: WILL */
#define TELOS_CMD_WONT    252   /* Negotiation: WONT */
#define TELOS_CMD_DO      253   /* Negotiation: DO */
#define TELOS_CMD_DONT    254   /* Negotiation: DONT */
#define TELOS_CMD_IAC     255   /* Interpret As Command escape byte */

/* RFC-numbered Telnet option codes used by callers building policy
   tables. Telos itself is option-agnostic; these are conveniences. */
#define TELOS_OPT_BINARY     0   /* RFC 856 */
#define TELOS_OPT_ECHO       1   /* RFC 857 */
#define TELOS_OPT_SGA        3   /* RFC 858 */
#define TELOS_OPT_STATUS     5   /* RFC 859 */
#define TELOS_OPT_TIMING     6   /* RFC 860 */
#define TELOS_OPT_TTYPE     24   /* RFC 1091 */
#define TELOS_OPT_NAWS      31   /* RFC 1073 */
#define TELOS_OPT_LINEMODE  34   /* RFC 1184 */

/* Subnegotiation buffer size. Caller-fixed at compile time so the
   session struct is fully POD. 256 bytes covers TTYPE-IS names,
   NAWS dimensions, and most realistic option subneg payloads. */
#define TELOS_SB_MAX 256

/* === Types ========================================================== */

/* Session role. Used only to seed default policy decisions; the
   protocol itself is symmetric in WILL/WONT/DO/DONT. */
typedef enum {
    TELOS_ROLE_CLIENT = 0,   /* we initiate; talking to a server */
    TELOS_ROLE_SERVER = 1    /* we listen; talking to a client */
} telos_role_t;

/* RFC 1143 Q-method per-option agreement state. Encoded as uint8 in
   the session struct so the 256-entry arrays cost 256 bytes each. */
typedef uint8 telos_q_t;
#define TELOS_Q_NO       ((telos_q_t)0)   /* option is not enabled */
#define TELOS_Q_YES      ((telos_q_t)1)   /* option is enabled, agreed */
#define TELOS_Q_WANTYES  ((telos_q_t)2)   /* we asked YES, awaiting reply */
#define TELOS_Q_WANTNO   ((telos_q_t)3)   /* we asked NO,  awaiting reply */

/* Direction for an option: applies to whether WE do it (local) or
   whether the PEER does it (remote). Used in policy callbacks and
   query functions to disambiguate. */
typedef enum {
    TELOS_DIR_LOCAL  = 0,   /* "us[option]" — we are the one doing it */
    TELOS_DIR_REMOTE = 1    /* "him[option]" — peer is the one doing it */
} telos_direction_t;

/* Configuration flags, bitwise-OR'd at telos_init time. */
#define TELOS_FLAG_NVT_LINE_ENDING  (1u << 0)
    /* RFC 854: CR LF and CR NUL both encode a bare CR in NVT mode.
       When this flag is set AND BINARY is not YES in the receive
       direction, the LF/NUL after a CR is consumed silently so the
       caller's data stream contains just a CR. Recommended unless the
       caller wants byte-faithful CR LF / CR NUL preservation. */

#define TELOS_FLAG_STRICT_PEER      (1u << 1)
    /* Emit TELOS_EV_PROTO_ERROR when the peer violates negotiation
       rules per a strict reading of RFC 855 (e.g. re-WILL on YES,
       SB with unmatched SE, IAC followed by an unrecognised verb).
       Default off. Most production callers want Postel-style
       tolerance; tests can flip this on. */

/* Event types delivered through the event callback. */
typedef enum {
    TELOS_EV_DATA,           /* User-data bytes (post-IAC-strip, post-NVT-normalise) */
    TELOS_EV_COMMAND,        /* IAC <single-byte-cmd>: NOP/IP/AO/AYT/EC/EL/GA/BRK/DM */
    TELOS_EV_OPTION_ENABLED, /* Option transitioned NO -> YES in a direction */
    TELOS_EV_OPTION_DISABLED,/* Option transitioned YES -> NO in a direction */
    TELOS_EV_SUBNEG,         /* SB <opt> body IAC SE received; body is IAC-decoded */
    TELOS_EV_PROTO_ERROR     /* (TELOS_FLAG_STRICT_PEER only) peer violation */
} telos_event_type_t;

/* Single event handed to the callback. Discriminated union via the
   `type` field. C89 doesn't allow designated initialisers; consumers
   read the field that matches `type`. */
typedef struct telos_event {
    telos_event_type_t type;
    union {
        struct {
            const uint8 *bytes;
            uint32       len;
        } data;
        struct {
            uint8 cmd;     /* one of TELOS_CMD_NOP .. TELOS_CMD_GA */
        } command;
        struct {
            uint8             option;
            telos_direction_t direction;
        } option;
        struct {
            uint8        option;
            const uint8 *body;
            uint32       body_len;
        } subneg;
        struct {
            const char *reason;   /* short ASCII description */
            uint8       byte;     /* offending byte where meaningful */
        } proto_error;
    } u;
} telos_event_t;

/* Per-option policy callback. Called when the peer first proposes
   enabling an option in a given direction (state was Q_NO, peer sent
   WILL or DO). Return non-zero to agree (Telos sends DO/WILL back and
   transitions to YES); return zero to refuse (Telos sends DONT/WONT,
   stays NO). NOT called for re-proposals on YES — those are absorbed
   silently per Q-method. */
typedef int (*telos_policy_fn)(void              *ctx,
                               uint8              option,
                               telos_direction_t  direction);

/* Event callback. Called for each event the session produces during
   telos_recv() or other API calls. The event's pointer fields (bytes,
   body, reason) are valid only for the duration of the call; if the
   caller needs them later, copy. */
typedef void (*telos_event_fn)(void                *ctx,
                               const telos_event_t *ev);

/* Write callback. Telos calls this whenever it needs to emit bytes to
   the wire (negotiation replies, subneg bodies, escaped data). The
   caller writes them to its transport. Telos batches where natural
   but does not require the caller to buffer. */
typedef void (*telos_write_fn)(void        *ctx,
                               const uint8 *bytes,
                               uint32       len);

/* Parser states, exposed so the session struct is complete in the
   header (caller-allocated; no opaque pointers). Internal; do not
   inspect or modify. */
typedef enum {
    TELOS_PS_NORMAL = 0,
    TELOS_PS_AFTER_IAC,
    TELOS_PS_AFTER_VERB,
    TELOS_PS_IN_SB,
    TELOS_PS_IN_SB_AFTER_IAC
} telos_parser_state_t;

/* Session state. POD; caller allocates and passes to telos_init(). */
typedef struct telos_session {
    /* Configuration set at init time. */
    telos_role_t      role;
    uint32            flags;
    telos_policy_fn   policy_cb;
    telos_event_fn    event_cb;
    telos_write_fn    write_cb;
    void             *ctx;

    /* Parser state. */
    telos_parser_state_t state;
    uint8                verb;          /* WILL/WONT/DO/DONT we're waiting on */
    int                  last_was_cr;   /* for NVT line-end normalisation */

    /* RFC 1143 Q-method state for all 256 options, both directions. */
    telos_q_t us[256];                  /* are WE doing option N */
    telos_q_t him[256];                 /* is PEER doing option N */

    /* Subnegotiation receive buffer. Built up while IN_SB; emitted
       on IAC SE. */
    uint8  sb_option;
    uint8  sb_buf[TELOS_SB_MAX];
    uint32 sb_len;
} telos_session_t;

/* === API ============================================================ */

/* Initialise the session. All pointers may be NULL except `s`;
   callbacks that are NULL cause Telos to silently skip the
   corresponding action (e.g. NULL policy_cb is treated as "always
   refuse"). After init, the session is ready to feed bytes through
   telos_recv(). */
void telos_init(telos_session_t *s,
                telos_role_t      role,
                uint32            flags,
                telos_policy_fn   policy_cb,
                telos_event_fn    event_cb,
                telos_write_fn    write_cb,
                void             *ctx);

/* Feed received bytes through the parser. Events fire via event_cb;
   negotiation replies and other outbound bytes fire via write_cb. The
   `bytes` buffer is consumed in full before the function returns;
   nothing is retained. */
void telos_recv(telos_session_t *s,
                const uint8     *bytes,
                uint32           len);

/* Send application data bytes. Any 0xFF (IAC) in the buffer is
   doubled per RFC 854. If TELOS_FLAG_NVT_LINE_ENDING is set and
   BINARY is not YES in the local direction, CR bytes are emitted as
   CR LF (the NVT newline encoding); the caller passes plain CRs and
   Telos handles the wire encoding. Bytes are emitted via write_cb. */
void telos_send_data(telos_session_t *s,
                     const uint8     *bytes,
                     uint32           len);

/* Begin negotiating an option in the LOCAL direction. Emits IAC WILL
   <option> only if the Q-state is NO; transitions to WANTYES. No-op
   if already YES or WANTYES. */
void telos_offer_will(telos_session_t *s, uint8 option);

/* Begin negotiating an option in the REMOTE direction. Emits IAC DO
   <option> only if the Q-state is NO; transitions to WANTYES. No-op
   if already YES or WANTYES. */
void telos_offer_do(telos_session_t *s, uint8 option);

/* Withdraw an option in the LOCAL direction. Emits IAC WONT <option>
   if currently YES or WANTYES; transitions to WANTNO. No-op if NO. */
void telos_withdraw_will(telos_session_t *s, uint8 option);

/* Withdraw an option in the REMOTE direction. Emits IAC DONT <option>
   if currently YES or WANTYES; transitions to WANTNO. No-op if NO. */
void telos_withdraw_do(telos_session_t *s, uint8 option);

/* Send a subnegotiation block: IAC SB <option> body... IAC SE. Any
   0xFF byte in body is doubled per RFC 855. */
void telos_send_subneg(telos_session_t *s,
                       uint8            option,
                       const uint8     *body,
                       uint32           body_len);

/* Send a single-byte command (IAC followed by one of TELOS_CMD_NOP
   .. TELOS_CMD_GA, or IAC IAC for a literal 0xFF if you really need
   it). Returns silently if `cmd` is not a valid single-byte command. */
void telos_send_command(telos_session_t *s, uint8 cmd);

/* Query the current Q-state for an option in a direction. */
telos_q_t telos_q_state(const telos_session_t *s,
                        uint8                   option,
                        telos_direction_t       direction);

#endif  /* PADAWAN_TELOS_H */
