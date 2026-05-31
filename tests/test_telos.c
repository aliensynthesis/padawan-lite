/*
 * Unit tests for telos/telos.{h,c} — the Telnet protocol engine.
 *
 * Each scenario sets up a session with policy/event/write callbacks
 * that record into a shared context struct. After driving the session
 * via telos_recv() or telos_send_*() we inspect the recorded events
 * and write buffers to assert behaviour.
 */
#include <stdio.h>
#include <string.h>

#include "telos.h"

static int g_fail = 0;

#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); g_fail = 1; }     \
    else         { fprintf(stderr, "PASS: %s\n", (msg)); }                 \
} while (0)

/* === Recorder context shared by callbacks ============================ */

#define LOG_MAX 4096

typedef struct {
    /* Bytes the session asked us to write (accumulated). */
    uint8  written[LOG_MAX];
    uint32 written_len;
    /* Data events: bytes flushed as TELOS_EV_DATA. */
    uint8  data[LOG_MAX];
    uint32 data_len;
    /* Most recent command, option, subneg, error captured. */
    uint8  last_command;
    int    command_count;
    uint8  last_opt_enabled_option;
    int    last_opt_enabled_dir;     /* TELOS_DIR_* */
    int    opt_enabled_count;
    uint8  last_opt_disabled_option;
    int    last_opt_disabled_dir;
    int    opt_disabled_count;
    uint8  last_subneg_option;
    uint8  last_subneg_body[LOG_MAX];
    uint32 last_subneg_body_len;
    int    subneg_count;
    int    proto_error_count;
    /* Policy responses configured by the test. Index by option. */
    int    policy_yes_local[256];
    int    policy_yes_remote[256];
} recorder_t;

static void rec_reset(recorder_t *r)
{
    memset(r, 0, sizeof(*r));
}

static int policy_cb(void *ctx, uint8 option, telos_direction_t dir)
{
    recorder_t *r = (recorder_t *)ctx;
    if (dir == TELOS_DIR_LOCAL)  return r->policy_yes_local[option];
    if (dir == TELOS_DIR_REMOTE) return r->policy_yes_remote[option];
    return 0;
}

static void event_cb(void *ctx, const telos_event_t *ev)
{
    recorder_t *r = (recorder_t *)ctx;
    switch (ev->type) {
    case TELOS_EV_DATA:
        if (r->data_len + ev->u.data.len <= LOG_MAX) {
            memcpy(r->data + r->data_len, ev->u.data.bytes, ev->u.data.len);
            r->data_len += ev->u.data.len;
        }
        break;
    case TELOS_EV_COMMAND:
        r->last_command = ev->u.command.cmd;
        r->command_count++;
        break;
    case TELOS_EV_OPTION_ENABLED:
        r->last_opt_enabled_option = ev->u.option.option;
        r->last_opt_enabled_dir    = (int)ev->u.option.direction;
        r->opt_enabled_count++;
        break;
    case TELOS_EV_OPTION_DISABLED:
        r->last_opt_disabled_option = ev->u.option.option;
        r->last_opt_disabled_dir    = (int)ev->u.option.direction;
        r->opt_disabled_count++;
        break;
    case TELOS_EV_SUBNEG:
        r->last_subneg_option = ev->u.subneg.option;
        if (ev->u.subneg.body_len <= LOG_MAX) {
            memcpy(r->last_subneg_body, ev->u.subneg.body,
                   ev->u.subneg.body_len);
            r->last_subneg_body_len = ev->u.subneg.body_len;
        }
        r->subneg_count++;
        break;
    case TELOS_EV_PROTO_ERROR:
        r->proto_error_count++;
        break;
    }
}

static void write_cb(void *ctx, const uint8 *bytes, uint32 len)
{
    recorder_t *r = (recorder_t *)ctx;
    if (r->written_len + len <= LOG_MAX) {
        memcpy(r->written + r->written_len, bytes, len);
        r->written_len += len;
    }
}

static int bytes_eq(const uint8 *a, uint32 alen,
                    const uint8 *b, uint32 blen)
{
    return (alen == blen) && (memcmp(a, b, alen) == 0);
}

/* === Tests ========================================================== */

int main(void)
{
    recorder_t      r;
    telos_session_t s;

    /* === 1. init: clean state =========================================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0,
               policy_cb, event_cb, write_cb, &r);
    CHECK(s.state == TELOS_PS_NORMAL,
          "1a: parser state starts NORMAL");
    CHECK(telos_q_state(&s, TELOS_OPT_BINARY, TELOS_DIR_LOCAL) == TELOS_Q_NO &&
          telos_q_state(&s, TELOS_OPT_BINARY, TELOS_DIR_REMOTE) == TELOS_Q_NO,
          "1b: BINARY Q-state starts NO in both directions");
    CHECK(s.last_was_cr == 0, "1c: last_was_cr starts zero");

    /* === 2. plain data passes through ================================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    telos_recv(&s, (const uint8 *)"hello", 5);
    CHECK(bytes_eq(r.data, r.data_len, (const uint8 *)"hello", 5),
          "2:  plain data passes through unchanged");

    /* === 3. IAC IAC produces a literal 0xFF data byte ================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = { 'a', 0xFF, 0xFF, 'b' };
        static const uint8 expect[] = { 'a', 0xFF, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, expect, sizeof(expect)),
              "3:  IAC IAC decodes to literal 0xFF data byte");
    }

    /* === 4-7. NVT line-ending normalisation ============================ */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = { 'a', 0x0D, 0x0A, 'b' };
        static const uint8 expect[] = { 'a', 0x0D, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, expect, sizeof(expect)),
              "4:  NVT CR LF normalised to CR (LF dropped)");
    }
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = { 'a', 0x0D, 0x00, 'b' };
        static const uint8 expect[] = { 'a', 0x0D, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, expect, sizeof(expect)),
              "5:  NVT CR NUL normalised to CR (NUL dropped)");
    }
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = { 'a', 0x0A, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, in, sizeof(in)),
              "6:  Standalone LF (no CR before) passes through unchanged");
    }
    /* BINARY YES suppresses NVT normalisation. */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    s.him[TELOS_OPT_BINARY] = TELOS_Q_YES;
    {
        static const uint8 in[] = { 'a', 0x0D, 0x0A, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, in, sizeof(in)),
              "7:  BINARY YES on remote suppresses NVT normalisation");
    }

    /* === 8. flag off: CR LF preserved (caller wants raw bytes) ========== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = { 'a', 0x0D, 0x0A, 'b' };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.data, r.data_len, in, sizeof(in)),
              "8:  NVT flag off: CR LF preserved verbatim");
    }

    /* === 9. single-byte commands surfaced via TELOS_EV_COMMAND ========= */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[] = {
            0xFF, TELOS_CMD_NOP, 0xFF, TELOS_CMD_IP,
            0xFF, TELOS_CMD_AYT, 0xFF, TELOS_CMD_GA
        };
        telos_recv(&s, in, sizeof(in));
        CHECK(r.command_count == 4,
              "9a: four IAC <cmd> sequences yield four COMMAND events");
        CHECK(r.last_command == TELOS_CMD_GA,
              "9b: last command captured is GA");
    }

    /* === 10. telos_send_command emits IAC <cmd> on wire ================= */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    telos_send_command(&s, TELOS_CMD_AYT);
    {
        static const uint8 expect[] = { 0xFF, TELOS_CMD_AYT };
        CHECK(bytes_eq(r.written, r.written_len, expect, sizeof(expect)),
              "10: send_command(AYT) writes IAC AYT");
    }

    /* === 11. WILL X + policy yes -> DO X sent, him=YES, event fires ==== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    r.policy_yes_remote[TELOS_OPT_BINARY] = 1;
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_BINARY };
        static const uint8 expect_out[] = { 0xFF, TELOS_CMD_DO, TELOS_OPT_BINARY };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect_out, sizeof(expect_out)),
              "11a: WILL + policy yes -> DO sent");
        CHECK(telos_q_state(&s, TELOS_OPT_BINARY, TELOS_DIR_REMOTE) == TELOS_Q_YES,
              "11b: him[BINARY] transitioned to YES");
        CHECK(r.opt_enabled_count == 1 &&
              r.last_opt_enabled_option == TELOS_OPT_BINARY &&
              r.last_opt_enabled_dir == TELOS_DIR_REMOTE,
              "11c: TELOS_EV_OPTION_ENABLED fired for remote BINARY");
    }

    /* === 12. WILL X + policy no -> DONT X sent, him stays NO =========== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    /* policy_yes_remote[ECHO] left at 0 = refuse */
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_ECHO };
        static const uint8 expect_out[] = { 0xFF, TELOS_CMD_DONT, TELOS_OPT_ECHO };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect_out, sizeof(expect_out)),
              "12a: WILL + policy no -> DONT sent");
        CHECK(telos_q_state(&s, TELOS_OPT_ECHO, TELOS_DIR_REMOTE) == TELOS_Q_NO,
              "12b: him[ECHO] stays NO after refusal");
        CHECK(r.opt_enabled_count == 0,
              "12c: no OPTION_ENABLED event on refusal");
    }

    /* === 13. DO X + policy yes -> WILL X sent, us=YES, event fires ===== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    r.policy_yes_local[TELOS_OPT_SGA] = 1;
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_DO, TELOS_OPT_SGA };
        static const uint8 expect_out[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_SGA };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect_out, sizeof(expect_out)),
              "13a: DO + policy yes -> WILL sent");
        CHECK(telos_q_state(&s, TELOS_OPT_SGA, TELOS_DIR_LOCAL) == TELOS_Q_YES,
              "13b: us[SGA] transitioned to YES");
        CHECK(r.opt_enabled_count == 1 &&
              r.last_opt_enabled_dir == TELOS_DIR_LOCAL,
              "13c: OPTION_ENABLED event direction = LOCAL");
    }

    /* === 14. offer_will + recv DO -> WANTYES -> YES silent transition == */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    telos_offer_will(&s, TELOS_OPT_SGA);
    {
        static const uint8 expect_offer[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_SGA };
        CHECK(bytes_eq(r.written, r.written_len, expect_offer, sizeof(expect_offer)),
              "14a: offer_will writes IAC WILL <option>");
    }
    CHECK(telos_q_state(&s, TELOS_OPT_SGA, TELOS_DIR_LOCAL) == TELOS_Q_WANTYES,
          "14b: us[SGA] transitioned NO -> WANTYES");
    /* Peer acks with DO. Should not trigger another write. */
    r.written_len = 0;
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_DO, TELOS_OPT_SGA };
        telos_recv(&s, in, sizeof(in));
    }
    CHECK(r.written_len == 0,
          "14c: WANTYES -> YES on incoming DO is silent (loop-breaker)");
    CHECK(telos_q_state(&s, TELOS_OPT_SGA, TELOS_DIR_LOCAL) == TELOS_Q_YES,
          "14d: us[SGA] transitioned WANTYES -> YES");

    /* === 15. offer_will is idempotent when already in YES ============== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    s.us[TELOS_OPT_SGA] = TELOS_Q_YES;
    telos_offer_will(&s, TELOS_OPT_SGA);
    CHECK(r.written_len == 0,
          "15: offer_will is no-op when state already YES");

    /* === 16. WONT on YES -> DONT sent, event fires ===================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    s.him[TELOS_OPT_BINARY] = TELOS_Q_YES;
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_WONT, TELOS_OPT_BINARY };
        static const uint8 expect_out[] = { 0xFF, TELOS_CMD_DONT, TELOS_OPT_BINARY };
        telos_recv(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect_out, sizeof(expect_out)),
              "16a: WONT on YES -> DONT sent");
        CHECK(r.opt_disabled_count == 1 &&
              r.last_opt_disabled_option == TELOS_OPT_BINARY,
              "16b: OPTION_DISABLED event fires");
    }

    /* === 17. Subneg receive: framing + body extraction ================ */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        /* IAC SB NAWS 0x00 0x50 0x00 0x18 IAC SE  (80x24) */
        static const uint8 in[] = {
            0xFF, TELOS_CMD_SB, TELOS_OPT_NAWS,
            0x00, 0x50, 0x00, 0x18,
            0xFF, TELOS_CMD_SE
        };
        static const uint8 expect_body[] = { 0x00, 0x50, 0x00, 0x18 };
        telos_recv(&s, in, sizeof(in));
        CHECK(r.subneg_count == 1 &&
              r.last_subneg_option == TELOS_OPT_NAWS &&
              bytes_eq(r.last_subneg_body, r.last_subneg_body_len,
                       expect_body, sizeof(expect_body)),
              "17: SB NAWS body delivered to subneg event");
    }

    /* === 18. Subneg receive: IAC IAC inside body decodes to 0xFF ======= */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        /* SB OPT 0x01 IAC IAC 0x02 IAC SE -> body { 0x01, 0xFF, 0x02 } */
        static const uint8 in[] = {
            0xFF, TELOS_CMD_SB, 99,
            0x01, 0xFF, 0xFF, 0x02,
            0xFF, TELOS_CMD_SE
        };
        static const uint8 expect_body[] = { 0x01, 0xFF, 0x02 };
        telos_recv(&s, in, sizeof(in));
        CHECK(r.subneg_count == 1 &&
              r.last_subneg_option == 99 &&
              bytes_eq(r.last_subneg_body, r.last_subneg_body_len,
                       expect_body, sizeof(expect_body)),
              "18: IAC IAC inside SB body decodes to literal 0xFF");
    }

    /* === 19. Subneg send: framing + IAC IAC encoding ================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 body[]   = { 0x01, 0xFF, 0x02 };
        static const uint8 expect[] = {
            0xFF, TELOS_CMD_SB, 99,
            0x01, 0xFF, 0xFF, 0x02,
            0xFF, TELOS_CMD_SE
        };
        telos_send_subneg(&s, 99, body, sizeof(body));
        CHECK(bytes_eq(r.written, r.written_len, expect, sizeof(expect)),
              "19: send_subneg frames body, doubles 0xFF");
    }

    /* === 20-22. Strict-peer mode emits proto errors ==================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_STRICT_PEER,
               policy_cb, event_cb, write_cb, &r);
    s.him[TELOS_OPT_BINARY] = TELOS_Q_YES;
    {
        /* Peer re-WILLs an option already YES — protocol violation. */
        static const uint8 in[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_BINARY };
        telos_recv(&s, in, sizeof(in));
        CHECK(r.proto_error_count == 1,
              "20: STRICT_PEER: redundant WILL on YES -> proto error");
    }
    /* Default mode: redundant WILL is silently absorbed. */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    s.him[TELOS_OPT_BINARY] = TELOS_Q_YES;
    {
        static const uint8 in[] = { 0xFF, TELOS_CMD_WILL, TELOS_OPT_BINARY };
        telos_recv(&s, in, sizeof(in));
        CHECK(r.proto_error_count == 0 && r.written_len == 0,
              "21: tolerant: redundant WILL on YES -> silent absorb");
    }

    /* === 23. send_data doubles IAC in user data ======================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[]     = { 'a', 0xFF, 'b' };
        static const uint8 expect[] = { 'a', 0xFF, 0xFF, 'b' };
        telos_send_data(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect, sizeof(expect)),
              "23: send_data doubles literal 0xFF in user data");
    }

    /* === 24. send_data with NVT flag encodes CR as CR LF ============== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    {
        static const uint8 in[]     = { 'a', 0x0D, 'b' };
        static const uint8 expect[] = { 'a', 0x0D, 0x0A, 'b' };
        telos_send_data(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, expect, sizeof(expect)),
              "24: NVT send_data: CR encoded as CR LF on wire");
    }

    /* === 25. send_data with NVT flag but BINARY YES locally: CR raw === */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, TELOS_FLAG_NVT_LINE_ENDING,
               policy_cb, event_cb, write_cb, &r);
    s.us[TELOS_OPT_BINARY] = TELOS_Q_YES;
    {
        static const uint8 in[] = { 'a', 0x0D, 'b' };
        telos_send_data(&s, in, sizeof(in));
        CHECK(bytes_eq(r.written, r.written_len, in, sizeof(in)),
              "25: BINARY YES locally suppresses CR LF encoding on send");
    }

    /* === 26. Split sequence across two recv calls ==================== */
    rec_reset(&r);
    telos_init(&s, TELOS_ROLE_SERVER, 0, policy_cb, event_cb, write_cb, &r);
    r.policy_yes_remote[TELOS_OPT_BINARY] = 1;
    {
        /* First recv: IAC WILL ; second recv: BINARY. Engine must
           carry state across calls. */
        static const uint8 part1[] = { 0xFF, TELOS_CMD_WILL };
        static const uint8 part2[] = { TELOS_OPT_BINARY };
        telos_recv(&s, part1, sizeof(part1));
        CHECK(r.written_len == 0,
              "26a: partial IAC WILL across two recvs: nothing yet");
        telos_recv(&s, part2, sizeof(part2));
        CHECK(r.written_len == 3 &&
              telos_q_state(&s, TELOS_OPT_BINARY, TELOS_DIR_REMOTE)
                  == TELOS_Q_YES,
              "26b: continuation completes negotiation, DO sent");
    }

    /* === 27. NULL callbacks tolerated ================================ */
    {
        telos_session_t s2;
        static const uint8 in[] = { 'a', 0xFF, 0xFF, 'b', 0xFF, TELOS_CMD_NOP };
        telos_init(&s2, TELOS_ROLE_CLIENT, 0, NULL, NULL, NULL, NULL);
        /* No callbacks: should not crash, no writes attempted, no
           events delivered, no policy queries. Drive a representative
           stream through and just confirm the session survives. */
        telos_recv(&s2, in, sizeof(in));
        telos_send_data(&s2, in, sizeof(in));
        telos_offer_will(&s2, TELOS_OPT_SGA);
        CHECK(s2.state == TELOS_PS_NORMAL,
              "27: NULL callbacks tolerated, session survives");
    }

    if (g_fail) {
        fprintf(stderr, "test_telos: FAILED\n");
        return 1;
    }
    fprintf(stderr, "test_telos: all scenarios pass\n");
    return 0;
}
