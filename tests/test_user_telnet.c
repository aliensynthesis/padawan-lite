/*
 * Unit test for bridge/user_telnet.c — verifies RFC 1143 Q-method
 * convergence. The bug this defends against: a stateless peer (e.g.
 * tcpser) that mirrors every WILL/DO with an unconditional DO/WILL
 * would, against a stateless user_telnet, cause an infinite
 * negotiation loop. With Q-state we converge in one round and
 * silently ignore redundant acknowledgements.
 *
 * Scenarios:
 *   1. send_initial writes exactly the 6 commands we offer.
 *   2. Peer's ack cluster (DO/WILL for each offered option) is
 *      consumed silently — no reply bytes.
 *   3. A duplicated ack cluster also produces no replies.
 *   4. A peer that unsolicited-offers WILL ECHO (refused by policy)
 *      gets one IAC DONT ECHO back.
 *   5. send_initial called twice is a no-op the second time.
 *   6. Reversed startup (peer offers first, we haven't sent
 *      initial): we reply once per option per policy and converge.
 *   7. RFC 854 line-ending normalisation:
 *      - CR LF emits one byte (CR), drops the LF.
 *      - CR NUL emits one byte (CR), drops the NUL.
 *      - Standalone LF (no preceding CR) passes through unchanged.
 *      - CR LF split across two calls is normalised across the
 *        boundary via last_was_cr.
 *      - CR followed by a literal 0xFF via IAC IAC: 0xFF resets
 *        the CR-trailing state; a subsequent LF is preserved as
 *        data (it is no longer immediately after a CR).
 */
#define _POSIX_C_SOURCE 200809L
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "user_telnet.h"

static int g_fail = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); g_fail = 1; } \
    else         { fprintf(stderr, "PASS: %s\n", (msg)); }             \
} while (0)

/* Drain all available bytes from a socket without blocking. */
static int drain(int fd, uint8 *buf, int max)
{
    int     flags;
    int     total;
    ssize_t n;

    total = 0;
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    for (;;) {
        if (total >= max) break;
        n = read(fd, buf + total, (size_t)(max - total));
        if (n > 0)      total += (int)n;
        else if (n == 0) break;
        else             break;   /* EAGAIN / EWOULDBLOCK */
    }
    fcntl(fd, F_SETFL, flags);
    return total;
}

int main(void)
{
    int            sv[2];
    user_telnet_t  t;
    uint8          buf[256];
    uint8          data_out[256];
    uint32         dlen;
    int            n;

    /* Peer "ack" cluster: the natural reply to our 6 initial offers.
       Order: WILL→DO/DONT mirror, DO→WILL/WONT mirror. */
    static const uint8 ack_cluster[] = {
        0xFF, 0xFD, 0x01,    /* DO ECHO   - reply to our WILL ECHO   */
        0xFF, 0xFD, 0x03,    /* DO SGA    - reply to our WILL SGA    */
        0xFF, 0xFB, 0x03,    /* WILL SGA  - reply to our DO SGA      */
        0xFF, 0xFD, 0x00,    /* DO BIN    - reply to our WILL BIN    */
        0xFF, 0xFB, 0x00,    /* WILL BIN  - reply to our DO BIN      */
        0xFF, 0xFB, 0x1F     /* WILL NAWS - reply to our DO NAWS     */
    };
    static const uint8 peer_will_echo[] = { 0xFF, 0xFB, 0x01 };

    /* Peer's spontaneous offer cluster (mirror of the bytes we'd
       send in send_initial, but used in scenario 6 to validate the
       "peer initiates" path). */
    static const uint8 peer_offer[] = {
        0xFF, 0xFB, 0x01,    /* WILL ECHO */
        0xFF, 0xFB, 0x03,    /* WILL SGA  */
        0xFF, 0xFD, 0x03,    /* DO SGA    */
        0xFF, 0xFB, 0x00,    /* WILL BIN  */
        0xFF, 0xFD, 0x00,    /* DO BIN    */
        0xFF, 0xFB, 0x1F     /* WILL NAWS */
    };

    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0) {
        fprintf(stderr, "socketpair failed: %s\n", strerror(errno));
        return 1;
    }

    /* === Scenario 1-5: we send initial first, peer acks =========== */
    user_telnet_init(&t, sv[0]);

    /* 1. send_initial writes 6 commands = 18 bytes. */
    user_telnet_send_initial(&t);
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 18, "1: initial cluster is 18 bytes (6 commands)");

    /* 2. Peer ack cluster: 18 bytes of IAC consumed, no data, no reply. */
    dlen = user_telnet_filter(&t, ack_cluster, sizeof(ack_cluster), data_out);
    CHECK(dlen == 0, "2a: no data bytes from pure-IAC input");
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 0,    "2b: peer's first ack cluster produces zero reply bytes");

    /* 3. Duplicate the ack cluster: still no reply (state == YES). */
    dlen = user_telnet_filter(&t, ack_cluster, sizeof(ack_cluster), data_out);
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 0,    "3:  duplicate ack cluster still produces zero reply bytes");

    /* 4. Peer offers ECHO (we never want peer to echo). One reply: DONT ECHO. */
    dlen = user_telnet_filter(&t, peer_will_echo, sizeof(peer_will_echo), data_out);
    CHECK(dlen == 0, "4a: peer WILL ECHO yields no data bytes");
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 3,    "4b: peer WILL ECHO yields exactly 3 reply bytes");
    if (n == 3) {
        CHECK(buf[0] == 0xFF && buf[1] == 0xFE && buf[2] == 0x01,
              "4c: reply is IAC DONT ECHO");
    }

    /* 5. send_initial again is a no-op (all options at YES/WANTYES). */
    user_telnet_send_initial(&t);
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 0,    "5:  second send_initial writes zero bytes");

    /* === Scenario 6: peer offers first (we never call send_initial) === */
    user_telnet_init(&t, sv[0]);

    dlen = user_telnet_filter(&t, peer_offer, sizeof(peer_offer), data_out);
    CHECK(dlen == 0, "6a: peer offer cluster yields no data bytes");
    n = drain(sv[1], buf, sizeof(buf));
    /* Expected replies:
       WILL ECHO  -> DONT ECHO  (refused)
       WILL SGA   -> DO   SGA   (accepted; him[SGA]=YES)
       DO   SGA   -> WILL SGA   (accepted; us[SGA]=YES)
       WILL BIN   -> DO   BIN
       DO   BIN   -> WILL BIN
       WILL NAWS  -> DO   NAWS
       = 6 replies = 18 bytes. */
    CHECK(n == 18,   "6b: peer offer yields 6 policy-driven replies (18 bytes)");

    /* Replay the same offer cluster. Now him[SGA]=YES and us[SGA]=YES
       et al, so those are silenced. ECHO stays at NO + refused so it
       gets DONT ECHO again (not a loop: peer's expected response to
       DONT is no reply, so the conversation terminates). */
    dlen = user_telnet_filter(&t, peer_offer, sizeof(peer_offer), data_out);
    n = drain(sv[1], buf, sizeof(buf));
    CHECK(n == 3,    "6c: duplicated offer only re-refuses ECHO (3 bytes)");

    /* === Scenario 7: RFC 854 CR LF / CR NUL normalisation ====== */
    user_telnet_init(&t, sv[0]);

    {
        static const uint8 cr_lf[]    = { 'c', ' ', '3', '0', '1', '0', '0',
                                          0x0D, 0x0A };
        static const uint8 cr_nul[]   = { 'd', 0x0D, 0x00 };
        static const uint8 lone_lf[]  = { 'x', 0x0A, 'y' };
        static const uint8 part1[]    = { 'a', 0x0D };
        static const uint8 part2[]    = { 0x0A, 'b' };
        static const uint8 cr_iaciac_lf[] = { 0x0D, 0xFF, 0xFF, 0x0A };
        uint8 out[64];

        dlen = user_telnet_filter(&t, cr_lf, sizeof(cr_lf), out);
        CHECK(dlen == 8 && out[dlen - 1] == 0x0D,
              "7a: CR LF emits 8 bytes ending in CR (LF dropped)");

        dlen = user_telnet_filter(&t, cr_nul, sizeof(cr_nul), out);
        CHECK(dlen == 2 && out[0] == 'd' && out[1] == 0x0D,
              "7b: CR NUL emits 2 bytes (NUL dropped)");

        dlen = user_telnet_filter(&t, lone_lf, sizeof(lone_lf), out);
        CHECK(dlen == 3 && out[1] == 0x0A,
              "7c: standalone LF passes through unchanged");

        /* Split CR LF across two filter calls: state must carry over. */
        dlen = user_telnet_filter(&t, part1, sizeof(part1), out);
        CHECK(dlen == 2 && out[1] == 0x0D,
              "7d: partial input ending in CR emits CR (LF not seen yet)");
        dlen = user_telnet_filter(&t, part2, sizeof(part2), out);
        CHECK(dlen == 1 && out[0] == 'b',
              "7e: continuation starting with LF drops it (CR LF straddled)");

        /* IAC IAC between CR and LF: the literal 0xFF data byte
           breaks the "immediately after CR" relationship, so the LF
           survives as data. */
        dlen = user_telnet_filter(&t, cr_iaciac_lf, sizeof(cr_iaciac_lf), out);
        CHECK(dlen == 3 && out[0] == 0x0D && out[1] == 0xFF && out[2] == 0x0A,
              "7f: CR, IAC IAC (literal 0xFF), LF -> all three preserved");
    }

    close(sv[0]);
    close(sv[1]);

    if (g_fail) {
        fprintf(stderr, "test_user_telnet: FAILED\n");
        return 1;
    }
    fprintf(stderr, "test_user_telnet: all scenarios pass\n");
    return 0;
}
