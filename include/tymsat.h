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

/* Stand-alone TYMSAT: the asynchronous terminal interface to a TYMNET
   network.

   WHY THIS IS NOT A PAD PERSONALITY
   ---------------------------------
   The TYMSAT occupies the same position in TYMNET that an X.28 PAD
   occupies in Telenet -- it is what a dial-up terminal actually talks
   to -- but it is not a PAD and cannot be modelled as one. TYMNET's
   own documentation is explicit (Network Products Concepts and
   Facilities, X.25/X.75 chapter, p. 6-4):

       "The TYMSAT is a nonpacket mode, asynchronous DTE that
        originates calls. A TYMSAT provides many of the features of a
        CCITT-defined PAD, as well as additional features."

   That is, from the network's point of view the TYMSAT is the DTE
   side -- the thing a PAD talks TO. The CCITT PAD function in TYMNET
   lives in the X.25/X.75 interface product as TPAD/HPAD, not in the
   TYMSAT. Padawan-Lite's libpadawancore X.28 PAD is the analogue of
   TYMNET's TPAD; this module is the analogue of the TYMSAT, and the
   two are peers rather than layers.

   Three further facts rule out the personality approach outright:

     1. Tables 5-1 and 5-2 (pp. 5-6, 5-7) list the X.28 PAD as an ISIS
        TYMSAT capability. It is absent from the stand-alone ("solo")
        TYMSAT this module emulates. The stand-alone TYMSAT is by
        definition the non-PAD path.

     2. "Interactive access to the TYMSAT for changing options is not
        possible while the program is running" (p. 5-10). There is no
        runtime command surface at all -- no command mode, no recall
        character, no X.3 SET/PAR. Options are baked into the Tymfile
        at system generation. personality_t's command_aliases,
        extended_param_ids and profile_overlay have nothing to bind to.

     3. TYMNET is a proprietary non-X.25 network; its X.25 support is
        ancillary and lives in a separate product. The X.28/X.29
        service-signal vocabulary personality_t overrides simply does
        not exist here.

   personality_by_name("tymnet") therefore still returns NULL, and the
   regression guard in tests/test_personality.c asserting that stays.
   The bridge routes --emulate tymnet to this module ahead of the
   personality lookup.

   SOURCES
   -------
   Two complementary primary sources, both in kb/:

     [HTU82]  "How to Use TYMNET -- A Reference Guide for Terminal
              Users", July 1982 (kb/How_to_Use_TYMNET_1982_OCR.txt).
              Authority for the wire: prompt text, the terminal-
              identifier table, the control characters, and the
              message catalogue. Cited below by line number.

     [NPCF85] "Network Products Concepts and Facilities", January 15
              1985 (Publication 57) plus the July 31 1985 update
              (NPD-57-1). Authority for architecture and the login
              model. Cited below by the document's own page numbers
              (e.g. p. 5-8), which are stable across the OCR.

   SCOPE
   -----
   Stand-alone (solo) TYMSAT only, per Table 5-1. In scope: PVC and
   MPVC ports, printer ports, Telex "Extension Cord". Out of scope:
   SIO ports, addressable ports, LISA, Videotext, Baudot (except
   50-baud for Telex), and the X.28 PAD -- all ISIS-only.

   Supervisor-side validation (MUD lookup, access profiles, classes
   and groups) is NOT modelled: NETVAL is an ISIS product and the
   real Supervisor is a network element we have no counterpart for.
   Username/host acceptance is driven entirely by tymnet.cfg, which
   plays the role the Tymfile plays in a real installation -- a
   generation-time configuration the running program cannot alter.
   Messages whose real trigger is a Supervisor-side condition are
   still in the catalogue below so the text is preserved, but are
   marked as not emitted by this emulation.

   LOGIN STRING GRAMMAR
   --------------------
   We implement the native TYMNET form only:

       <username>[:<host number>]<CR>

   The colon is [NPCF85 p. 14-6], which is the only separator either
   1985 document names: "Only users with passwords can use the colon
   (:) to specify a destination host", and "the network user is not
   required to specify a destination by entering a colon (:) and a
   host number in the login string". The word "semicolon" appears
   nowhere in either 1985 document.

   [HTU82] does show semicolons -- "INFORMATION;" at line 7 and
   "DPAC;;3020<host address>:<user data>" at line 88 -- but both are
   gateway forms: the second is explicitly the Datapac/X.25-X.75
   interface, where the colon carries X.25 call user data rather than
   a TYMNET host number. TYMNET's X.25 support is an ancillary product
   living outside the stand-alone TYMSAT, so the gateway login form is
   deliberately NOT implemented, and the two messages that only that
   path can raise (RE-ENTER ADDRESS AND DATA, TYPE "P" AHEAD OF
   ADDRESS) are marked NOT EMITTED in the catalogue below.

   VERIFY: that the semicolon forms are gateway-only, rather than an
   alternative native separator that the 1985 documents simply omit,
   is inference from context and not stated by either source. */
#ifndef PADAWAN_TYMSAT_H
#define PADAWAN_TYMSAT_H

#include "types.h"
#include "x25.h"

#define TYMSAT_USERNAME_MAX   31
#define TYMSAT_PASSWORD_MAX   31
#define TYMSAT_HOSTNUM_MAX    15  /* destination host number, as text */
#define TYMSAT_LOGIN_BUF      95  /* whole login string incl. separators */
#define TYMSAT_PENDING_SIZE  256  /* DTE bytes held during circuit build */

/* Login-inactivity limit. [HTU82:262] PLS SEE YOUR REP: "A valid user
   name or password has not been entered within two minutes following
   the terminal identifier entry." Unit is twentieths of a second to
   match pad_tick's convention: 120 s * 20 = 2400. */
#define TYMSAT_LOGIN_TIMEOUT_20THS  2400UL

/* Session states.
   Derived from the login procedure in [HTU82:24-61] and the five
   elements of login information in [NPCF85 p. 5-8]. Unlike
   pad_state_t these numbers carry no external significance -- there
   is no TYMNET equivalent of X.28's numbered state figures. */
typedef enum {
    /* Carrier up, nothing sent yet. Emitting the terminal-identifier
       request moves us to AWAITING_TID. */
    TYMSAT_STATE_IDLE            = 0,

    /* "please type your terminal identifier" has been sent. We are
       waiting for ONE character, not a line: [HTU82:30] "Enter your
       terminal identifier character." The P identifier is the sole
       exception and takes a following CR ([HTU82:137]). */
    TYMSAT_STATE_AWAITING_TID    = 1,

    /* TID accepted; node/port line and "please log in:" sent. We are
       accumulating the login string, which may carry embedded control
       characters ([NPCF85 p. 5-8] element 2). */
    TYMSAT_STATE_AWAITING_LOGIN  = 2,

    /* "password:" sent. Echo is suppressed for the duration:
       [HTU82:47] "Passwords are not displayed at full-duplex
       terminals for security reasons." Skipped entirely when the
       configured user entry carries no password. */
    TYMSAT_STATE_AWAITING_PASSWORD = 3,

    /* Circuit build in progress (x25_call returned X25_IN_PROGRESS).
       DTE bytes arriving here are buffered in pending[]. */
    TYMSAT_STATE_CIRCUIT_BUILD   = 4,

    /* Connected to the host; acceptance message has been emitted.
       Bytes pass through in both directions. */
    TYMSAT_STATE_DATA_TRANSFER   = 5,

    /* Circuit gone. [HTU82:57-61] logoff does NOT drop carrier: the
       user is returned to "please log in:" and may log in again or
       hang up. This state is transient -- it exists so the caller can
       observe the reason before we re-enter AWAITING_LOGIN. */
    TYMSAT_STATE_CLEARED         = 6
} tymsat_state_t;

/* Terminal identifier.

   [NPCF85 p. 5-8]: "The single-character TID identifies specific
   terminal operating characteristics to the TYMSAT. The user enters
   the TID upon login. The operating characteristics defined by the
   TID are baud rate, character set, carriage return delay, and
   echoing requirements."

   The concrete table is [HTU82:127-137]; the 1985 documents describe
   the mechanism but contain no table of identifier characters.

   RECORD-ONLY: the identifier is validated and stored, and nothing
   else. Accepting 'E' rather than 'A' does not currently change echo,
   pacing, or character set on the wire. This is deliberate --
   [HTU82] names the four characteristics a TID selects but quantifies
   none of them (no pad-character counts, no delay in milliseconds, no
   statement of which identifiers suppress echo), so making the TID
   bite would mean inventing numbers the sources do not contain. The
   fields below therefore describe the identifier; they do not yet
   configure the session.

   VERIFY: magnitudes for cr_delay, and the exact echo behaviour per
   identifier, are unsourced. cr_delay is a documented-or-not flag
   rather than a duration. Revisit if a Tymfile or CONSAT listing
   surfaces. */
typedef struct {
    /* The character the user types, e.g. 'A'. Uppercase; matching is
       case-insensitive. */
    uint8 id;

    /* Characters per second, as printed in [HTU82:129-137]. Where an
       identifier serves two rates ('A' is "30cps, 120cps") this holds
       the lower and cps_alt the higher; cps_alt is 0 otherwise. */
    uint16 cps;
    uint16 cps_alt;

    /* Non-zero when the identifier selects EBCD/Correspondence rather
       than ASCII. Only 'P' does so ([HTU82:137], Selectric-type
       terminals such as the 2741). */
    uint8 ebcd;

    /* Non-zero when this identifier must be followed by a carriage
       return. Only 'P' ([HTU82:137] renders it "P + carriage
       return"). */
    uint8 needs_cr;

    /* Non-zero when the identifier is documented as providing
       carriage-return delay ([HTU82:121-122]: E for 30cps terminals,
       I for 120cps). See the VERIFY above regarding magnitude. */
    uint8 cr_delay;

    /* Human-readable terminal class from [HTU82:129-137], for logs
       and for --list-tids. */
    const char *description;
} tymsat_tid_t;

/* Control characters embedded in the login string.

   [NPCF85 p. 5-8] element 2: "The user can specify operating
   characteristics for a session by imbedding control characters
   within the login string." The concrete set is [HTU82:108-115].
   These are session options, not editing keys: they are stripped
   from the login string as it is accumulated and recorded as flags.

   [HTU82:141] confirms placement: "enter Control R and Control X
   immediately before your user name." */
#define TYMSAT_CTL_HALF_DUPLEX  0x01  /* ^H: suppress TYMSAT echoing */
#define TYMSAT_CTL_EVEN_PARITY  0x02  /* ^P: even parity for output */
#define TYMSAT_CTL_TERM_FLOW    0x04  /* ^R: terminal may ^S/^Q the host */
#define TYMSAT_CTL_NET_FLOW     0x08  /* ^X: network ^S/^Q's the terminal */

/* ^S and ^Q are only meaningful once ^R (incoming) or ^X (outgoing)
   has been requested at login -- [HTU82:113-115] "Control S is
   effective only when a Control R has been entered at log in."
   Outside those modes they are ordinary data bytes. */
#define TYMSAT_XOFF  0x13
#define TYMSAT_XON   0x11

/* Message catalogue, [HTU82:196-289].

   Order follows the pamphlet. Entries marked NOT EMITTED correspond
   to conditions that only a real Supervisor, TYMCOM or physical
   network could raise; their text is retained so the catalogue stays
   complete and so a future transport can raise them, but nothing in
   this emulation produces them. */
typedef enum {
    TYMSAT_MSG_ACCESS_NOT_PERMITTED = 0,  /* bad username / barred node */
    TYMSAT_MSG_ALL_PORTS_BUSY,            /* NOT EMITTED */
    TYMSAT_MSG_BAD_HOST_NUMBER,           /* unknown host number */
    TYMSAT_MSG_BAD_MUD,                   /* NOT EMITTED (no MUD modelled) */
    TYMSAT_MSG_CIRCUITS_BUSY,             /* NOT EMITTED */
    TYMSAT_MSG_DATA_LOST_TOWARD_HOST,     /* NOT EMITTED */
    TYMSAT_MSG_DATA_LOST_TOWARD_TERMINAL, /* NOT EMITTED */
    TYMSAT_MSG_DROPPED_BY_HOST_SYSTEM,    /* remote closed the connection */
    TYMSAT_MSG_ERROR_TYPE_USER_NAME,      /* invalid username entered */
    TYMSAT_MSG_ERROR_TYPE_PASSWORD,       /* invalid password entered */
    TYMSAT_MSG_HOST_DOWN,                 /* TCP connect refused */
    TYMSAT_MSG_HOST_IS_ONLINE,            /* acceptance; see cfg accept_msg */
    TYMSAT_MSG_HOST_NOT_AVAILABLE,        /* "HOST NOT AVAILABLE THRU NET" */
    TYMSAT_MSG_HOST_NOT_RESPONDING,       /* connect timed out */
    TYMSAT_MSG_HOST_SHUT,                 /* NOT EMITTED */
    TYMSAT_MSG_LOGON_ABORTED,             /* NOT EMITTED */
    TYMSAT_MSG_NO_HOST_SPECIFIED,         /* multi-host user, no destination */
    TYMSAT_MSG_NO_PATH_AVAILABLE,         /* NOT EMITTED */
    TYMSAT_MSG_OUT_OF_CHANNELS,           /* NOT EMITTED */
    TYMSAT_MSG_PASSWORD,                  /* bare CR at the password prompt */
    TYMSAT_MSG_PLEASE_LOG_IN,             /* the login prompt itself */
    TYMSAT_MSG_PLS_SEE_YOUR_REP,          /* two-minute login timeout */
    TYMSAT_MSG_PLEASE_TRY_AGAIN,          /* multihost, wrong username */
    TYMSAT_MSG_REENTER_ADDRESS_AND_DATA,  /* NOT EMITTED (gateway, see below) */
    TYMSAT_MSG_SUBPROCESS_UNAVAILABLE,    /* NOT EMITTED */
    TYMSAT_MSG_RING_NO_ANSWER,            /* NOT EMITTED */
    TYMSAT_MSG_SYSTEM_ERROR_ON_PORT,      /* NOT EMITTED */
    TYMSAT_MSG_TEMPORARY_NETWORK_PROBLEM, /* unclassified transport error */
    TYMSAT_MSG_TRY_AGAIN_IN_2_MINUTES,    /* NOT EMITTED */
    TYMSAT_MSG_TYPE_P_AHEAD_OF_ADDRESS,   /* NOT EMITTED (gateway, see below) */
    TYMSAT_MSG_USER_NAME,                 /* bare CR at the login prompt */

    /* --- entries below are NOT from the [HTU82:196-289] catalogue --- */

    /* The terminal-identifier request of [HTU82:28]. A prompt from the
       procedural walkthrough rather than a catalogued message. */
    TYMSAT_MSG_TERMINAL_IDENTIFIER_REQ,

    /* Connect acknowledgement observed from a 1986 TYMNET client. The
       1982 pamphlet's catalogue has no such entry -- it documents only
       HOST IS ONLINE, plus a bare semicolon shown in the procedural
       walkthrough. See the accept_msg commentary below. */
    TYMSAT_MSG_CALL_CONNECTED,

    TYMSAT_MSG_COUNT
} tymsat_msg_t;

/* One configured user entry, from tymnet.cfg.

   This is the Tymfile's role, not NETVAL's: a generation-time table
   the running program cannot modify ([NPCF85 p. 5-10]). We keep only
   what changes the visible login dance. */
typedef struct {
    char username[TYMSAT_USERNAME_MAX + 1];

    /* Empty string = the No Password option ([NPCF85 p. 14-6]): the
       password prompt is skipped entirely. Note that in the real
       network No Password also forces Ignore Host; we honour that
       linkage only to the extent that such a user must have a
       default_host configured, since they cannot type a destination. */
    char password[TYMSAT_PASSWORD_MAX + 1];

    /* Destination used when the user supplies none. Empty means the
       user must specify one; omitting it then yields
       TYMSAT_MSG_NO_HOST_SPECIFIED. Models the "home" destination of
       [NPCF85 p. 14-10] without modelling access profiles. */
    char default_host[TYMSAT_HOSTNUM_MAX + 1];

    /* Non-zero = a destination typed by the user is discarded and
       default_host is always used. Models Ignore Host ([NPCF85
       p. 14-6]), which states the user "is not required to specify a
       destination" and that one is selected from the access profile
       instead. Note the source describes the destination as ignored,
       not as forbidden, so naming one is not an error here. */
    uint8 ignore_host;
} tymsat_user_t;

/* Connect-acknowledgement forms for tymsat_config_t.accept_msg.

   CALL_CONNECTED is deliberately 0 so that a zero-initialised config
   gets the client-attested form rather than one of the pamphlet's
   two examples. */
#define TYMSAT_ACCEPT_CALL_CONNECTED  0   /* "call connected" */
#define TYMSAT_ACCEPT_TERSE           1   /* ";" */
#define TYMSAT_ACCEPT_VERBOSE         2   /* "host is online" */

/* TYMSAT configuration -- the Tymfile analogue, loaded from
   tymnet.cfg. Everything here is fixed for the life of the process. */
typedef struct {
    /* Node and port reported in the line preceding the login prompt,
       [HTU82:34-37]:  -NNNN-PPP-  followed by "please log in:".

       VERIFY: [HTU82:36] renders this as "-NNNN-PPP-" and [HTU82:182]
       describes it as "the line of numbers and hyphens". The field
       widths in that rendering (4 and 3) are the pamphlet's own
       placeholders and are not stated as fixed-width anywhere, nor is
       it stated whether values are zero-padded. We emit the numbers
       unpadded between literal hyphens; revisit if a capture appears. */
    uint16 node_number;
    uint16 port_number;

    /* [HTU82:84]: "TYMNET displays three numbers in certain
       connections, such as those made via WATS lines. The first
       number in the sequence represents the node to which you are
       connected, the second is the number of the slot, and the third
       represents your port." Non-zero enables the three-number form.

       VERIFY: the pamphlet does not show the three-number form
       rendered, only describes it. We assume -NNNN-SS-PPP- by
       analogy with the two-number case. */
    uint8  emit_slot_number;
    uint16 slot_number;

    /* Message case on the wire.

       The sources conflict. [HTU82:28-59] renders prompts lowercase
       in what reads as verbatim terminal transcript ("please log
       in:", "password:", "host is online"). [HTU82:196-289] renders
       the same strings uppercase in the MESSAGES catalogue, and
       [NPCF85 p. 5-8] quotes the prompt uppercase as "PLEASE LOG
       IN:".

       Note what kind of context each is: both uppercase renderings
       are referential -- naming a message in prose -- while the only
       transcript-style rendering is lowercase. That is consistent
       with a lowercase wire and a period documentation convention of
       uppercasing message names. We therefore default to lowercase
       and make it switchable rather than committing hard.

       VERIFY: unresolved without a byte-level capture. 0 = lowercase
       (default), non-zero = uppercase. */
    uint8 uppercase_messages;

    /* Which connect acknowledgement to emit; one of TYMSAT_ACCEPT_*.

       Three forms are known, from two different kinds of evidence:

         "call connected"  Observed behaviour of a 1986 TYMNET client,
                           supplied by the project owner. Client-
                           observed rather than documented -- the same
                           class of evidence that fixed the PlayNET
                           address grouping and the telenet-91 recall
                           character, and historically the more
                           reliable of the two here. This is the
                           DEFAULT.

         ";"               [HTU82:49-53] procedural walkthrough.

         "host is online"  [HTU82:49-53] and the message catalogue at
                           [HTU82:231].

       The pamphlet introduces its pair with "an acceptance message,
       SUCH AS a semicolon (;) or 'host is online'" -- explicitly
       examples, not a closed set, and it never says what selects
       between them. A third form from a real client is therefore not
       in conflict with the documentation; it fills a gap the
       documentation left open.

       Note also that the two are not equally attested even within the
       pamphlet: "host is online" is a catalogued TYMNET message, while
       the semicolon appears only in the walkthrough and has no
       catalogue entry -- which may mean the semicolon was a host
       prompt rather than a network message. See deviations.txt.

       VERIFY: what selected between forms on the real network is
       unknown. We model it as a per-installation Tymfile setting. */
    uint8 accept_msg;

    /* Configured users. */
    const tymsat_user_t *users;
    uint16               user_count;
} tymsat_config_t;

/* Output callbacks, same shape as pad_emit_fn: emit_dte carries bytes
   toward the terminal, emit_remote carries bytes into the circuit. */
typedef void (*tymsat_emit_fn)(void *ctx, const uint8 *data, uint32 len);

typedef struct tymsat_session {
    tymsat_state_t state;

    /* Active configuration. Never NULL after tymsat_init; points to
       caller-owned storage that must outlive the session. */
    const tymsat_config_t *cfg;

    /* The accepted terminal identifier, or NULL before one is taken.
       Points into the static table in src/tymsat.c. */
    const tymsat_tid_t *tid;

    /* Session options accumulated from control characters embedded in
       the login string; a mask of TYMSAT_CTL_*. */
    uint8 ctl_flags;

    /* Login string accumulator and its parsed results. username and
       host_number are filled once CR terminates the string. */
    char  login_buf[TYMSAT_LOGIN_BUF + 1];
    uint8 login_len;
    char  username[TYMSAT_USERNAME_MAX + 1];
    char  host_number[TYMSAT_HOSTNUM_MAX + 1];

    /* Password accumulator. Never echoed ([HTU82:47]). */
    char  password[TYMSAT_PASSWORD_MAX + 1];
    uint8 password_len;

    /* The user entry matched at login, or NULL. */
    const tymsat_user_t *user;

    /* Twentieths of a second since the terminal identifier was
       accepted, reset on each accepted login field. Compared against
       TYMSAT_LOGIN_TIMEOUT_20THS. Frozen once DATA_TRANSFER is
       reached -- the two-minute limit is a login limit only. */
    uint32 login_ticks;

    /* DTE bytes received during CIRCUIT_BUILD, replayed on connect. */
    uint8  pending[TYMSAT_PENDING_SIZE];
    uint32 pending_len;

    /* Flow-control state, meaningful only when the corresponding
       TYMSAT_CTL_ flag is set. Non-zero = transmission is currently
       held off by an XOFF from that side. */
    uint8 host_flow_held;      /* ^R: terminal has XOFF'd the host */
    uint8 terminal_flow_held;  /* ^X: network has XOFF'd the terminal */

    x25_call_t     call;
    tymsat_emit_fn emit_dte;
    tymsat_emit_fn emit_remote;
    void          *ctx;
} tymsat_session_t;

/* Initialise a session and emit the terminal-identifier request.
   cfg must be non-NULL and outlive the session. Returns 0 on success,
   non-zero on bad arguments. */
int tymsat_init(tymsat_session_t *s,
                const tymsat_config_t *cfg,
                tymsat_emit_fn emit_dte,
                tymsat_emit_fn emit_remote,
                void *ctx);

/* Feed bytes from the terminal. Drives the login state machine while
   logging in; passes data into the circuit once connected. */
void tymsat_input_dte(tymsat_session_t *s, const uint8 *data, uint32 len);

/* Feed bytes arriving from the circuit toward the terminal. */
void tymsat_input_remote(tymsat_session_t *s, const uint8 *data, uint32 len);

/* Advance the login timer by elapsed_20ths twentieths of a second.
   Emits PLS SEE YOUR REP and returns non-zero when the two-minute
   limit ([HTU82:262]) expires; the caller should then drop carrier.
   A no-op once DATA_TRANSFER has been reached. */
int tymsat_tick(tymsat_session_t *s, uint32 elapsed_20ths);

/* Non-zero when this session has a timer that only tymsat_tick can
   advance, i.e. when it is waiting on elapsed time rather than on I/O.

   Event-loop drivers MUST consult this when choosing their poll
   timeout. A driver that blocks indefinitely whenever no descriptor is
   readable will never call tymsat_tick, and any state this session is
   counting down will never expire -- the two-minute login limit will
   not fire for an otherwise idle session. See deviations.txt. */
int tymsat_has_pending_timer(const tymsat_session_t *s);

/* Circuit-establishment results, called by the transport when
   tymsat_input_dte's x25_call returned X25_IN_PROGRESS. */
void tymsat_circuit_connected(tymsat_session_t *s);
void tymsat_circuit_failed(tymsat_session_t *s, tymsat_msg_t reason);

/* Remote end went away. Emits the appropriate message and returns the
   session to the login prompt WITHOUT dropping carrier ([HTU82:57-61]). */
void tymsat_circuit_cleared(tymsat_session_t *s, tymsat_msg_t reason);

/* Emit a catalogue message to the terminal, honouring the configured
   case. Exposed for the bridge, which raises transport-level
   conditions the session itself cannot detect. */
void tymsat_emit_message(tymsat_session_t *s, tymsat_msg_t msg);

/* Catalogue text in the canonical (lowercase) form, or NULL if msg is
   out of range. Does not apply the uppercase_messages setting -- that
   is applied at emit time. */
const char *tymsat_message_text(tymsat_msg_t msg);

/* Look up a terminal identifier character, case-insensitively.
   Returns NULL if the character is not a documented identifier. */
const tymsat_tid_t *tymsat_tid_by_char(uint8 c);

/* The identifier table and its length, for --list-tids and tests. */
const tymsat_tid_t *tymsat_tid_table(uint16 *count_out);

#endif
