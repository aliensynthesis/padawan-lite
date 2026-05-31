/*
 * Unit tests for bridge/term_id.{c,h}.
 *
 * Covers two surfaces:
 *
 *   (A) term_id_lookup() — case-insensitive table lookup, default
 *       resolution, unknown-name behaviour.
 *
 *   (B) term_id_filter_process() — the inline DA-query interceptor.
 *       Drives byte streams through the parser and asserts that:
 *         - DA1 (ESC [ c, ESC [ <param> c) is swallowed and the
 *           appropriate response goes to the response buffer
 *         - VT52 Identify (ESC Z) is swallowed and responded to
 *         - Non-query escapes (cursor moves, SGR) pass through
 *         - A query split across two calls is still recognised
 *         - DUMB profile produces no response (silent swallow OK
 *           because DA1/ESC Z responses are NULL for that entry)
 *         - Mixed plain-text + query yields the text plus a response
 *         - CSI overflow (very long parameter run) falls through
 *           cleanly via TIF_FLUSH_THROUGH
 *         - Multiple queries in one call produce concatenated responses
 */
#include <stdio.h>
#include <string.h>

#include "term_id.h"

static int g_fail = 0;

#define CHECK(cond, msg) do {                                              \
    if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); g_fail = 1; }     \
    else         { fprintf(stderr, "PASS: %s\n", (msg)); }                 \
} while (0)

static int bytes_eq(const uint8 *a, uint32 alen,
                    const uint8 *b, uint32 blen)
{
    if (alen != blen) return 0;
    return memcmp(a, b, alen) == 0;
}

int main(void)
{
    const term_id_entry_t *e;

    /* === (A) lookup ============================================ */
    e = term_id_lookup("VT100");
    CHECK(e != NULL && strcmp(e->name, "VT100") == 0,
          "lookup-uppercase: VT100 -> entry");
    e = term_id_lookup("vt100");
    CHECK(e != NULL && strcmp(e->name, "VT100") == 0,
          "lookup-lowercase: vt100 -> VT100");
    e = term_id_lookup("Vt220");
    CHECK(e != NULL && strcmp(e->name, "VT220") == 0,
          "lookup-mixed-case: Vt220 -> VT220");
    e = term_id_lookup("DUMB");
    CHECK(e != NULL && e->da1_response == NULL && e->escz_response == NULL,
          "lookup-DUMB: name found, both responses NULL");
    e = term_id_lookup("UNKNOWN");
    CHECK(e != NULL && strcmp(e->name, "UNKNOWN") == 0 &&
          e->da1_response == NULL && e->escz_response == NULL,
          "lookup-UNKNOWN: name found, both responses NULL");
    e = term_id_lookup("ansi");
    /* ANSI now mirrors VT100's DA1 (v1.5.6) so an --d1 ansi
       mapping doesn't trigger UNKTERM on the host side. ESC Z
       reply also present for symmetry with the VT-class entries. */
    CHECK(e != NULL && strcmp(e->name, "ANSI") == 0 &&
          e->da1_response != NULL && e->escz_response != NULL,
          "lookup-ansi (lc): resolves to ANSI, has DA1 + ESC Z responses");
    {
        static const uint8 expected_vt100_da1[] =
            { 0x1B, '[', '?', '1', ';', '0', 'c' };
        CHECK(e->da1_len == sizeof(expected_vt100_da1) &&
              memcmp(e->da1_response, expected_vt100_da1,
                     sizeof(expected_vt100_da1)) == 0,
              "ansi-da1: ANSI's DA1 equals VT100's (ESC [ ? 1 ; 0 c)");
    }
    e = term_id_lookup("BOGUS");
    CHECK(e == NULL, "lookup-unknown: returns NULL");
    e = term_id_lookup("");
    CHECK(e == NULL, "lookup-empty: returns NULL");
    e = term_id_lookup(NULL);
    CHECK(e == NULL, "lookup-null: returns NULL");

    e = term_id_default();
    CHECK(e != NULL && strcmp(e->name, "VT100") == 0,
          "default: VT100");

    /* === (B) filter ============================================ */
    {
        term_id_filter_t t;
        uint8 out[256];
        uint8 resp[64];
        uint32 resp_len;
        uint32 n;
        const term_id_entry_t *vt100 = term_id_lookup("VT100");
        const term_id_entry_t *vt220 = term_id_lookup("VT220");
        const term_id_entry_t *dumb  = term_id_lookup("DUMB");

        /* 1. Pass-through plain text. */
        term_id_filter_init(&t);
        n = term_id_filter_process(&t, vt100,
                                   (const uint8 *)"Hello", 5,
                                   out, resp, sizeof(resp), &resp_len);
        CHECK(n == 5 && bytes_eq(out, n, (const uint8 *)"Hello", 5),
              "filter-1a: plain text passes through unchanged");
        CHECK(resp_len == 0, "filter-1b: no response for plain text");

        /* 2. DA1 simple (ESC [ c). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', 'c' };
            static const uint8 expected[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0, "filter-2a: ESC [ c swallowed (no forwarded bytes)");
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-2b: response is VT100 DA1");
        }

        /* 3. DA1 with parameter (ESC [ 0 c). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', '0', 'c' };
            static const uint8 expected[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0, "filter-3a: ESC [ 0 c swallowed");
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-3b: response is VT100 DA1");
        }

        /* 4. VT52 Identify (ESC Z). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, 'Z' };
            static const uint8 expected[] = { 0x1B, '/', 'Z' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0, "filter-4a: ESC Z swallowed");
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-4b: response is ESC / Z (VT52 Identify reply)");
        }

        /* 5. DA1 split across two calls. */
        term_id_filter_init(&t);
        {
            static const uint8 in1[] = { 0x1B, '[' };
            static const uint8 in2[] = { 'c' };
            static const uint8 expected[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, vt100, in1, sizeof(in1),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0 && resp_len == 0,
                  "filter-5a: ESC [ alone produces nothing yet");
            n = term_id_filter_process(&t, vt100, in2, sizeof(in2),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0 &&
                  bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-5b: continuation 'c' completes the DA1 query");
        }

        /* 6. Non-query escape (cursor home: ESC [ H). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', 'H' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 3 && bytes_eq(out, n, in, sizeof(in)),
                  "filter-6a: ESC [ H passes through unchanged");
            CHECK(resp_len == 0, "filter-6b: no response for cursor home");
        }

        /* 7. SGR (ESC [ 1 ; 30 m). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] =
                { 0x1B, '[', '1', ';', '3', '0', 'm' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 7 && bytes_eq(out, n, in, sizeof(in)),
                  "filter-7a: SGR passes through unchanged");
            CHECK(resp_len == 0, "filter-7b: no response for SGR");
        }

        /* 8. Mixed text + DA1 + text. */
        term_id_filter_init(&t);
        {
            static const uint8 in[] =
                { 'A', 'B', 0x1B, '[', 'c', 'C', 'D' };
            static const uint8 expected_out[] = { 'A', 'B', 'C', 'D' };
            static const uint8 expected_resp[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 4 &&
                  bytes_eq(out, n, expected_out, sizeof(expected_out)),
                  "filter-8a: surrounding text preserved, query removed");
            CHECK(bytes_eq(resp, resp_len,
                           expected_resp, sizeof(expected_resp)),
                  "filter-8b: response is VT100 DA1");
        }

        /* 9. DUMB profile: swallows but no response. */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', 'c' };
            n = term_id_filter_process(&t, dumb, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0,
                  "filter-9a: DUMB still swallows the query");
            CHECK(resp_len == 0,
                  "filter-9b: DUMB emits no response (host falls back)");
        }

        /* 9c. UNKNOWN behaves like DUMB at the filter level (NULL
           DA1/ESC Z); the difference is in the TTYPE-IS name only. */
        {
            const term_id_entry_t *unknown = term_id_lookup("UNKNOWN");
            static const uint8 in[] = { 0x1B, '[', 'c' };

            term_id_filter_init(&t);
            n = term_id_filter_process(&t, unknown, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0 && resp_len == 0,
                  "filter-9c: UNKNOWN swallows DA query, no response");
        }

        /* 9d. ANSI: since v1.5.6 ANSI mirrors VT100's DA1 reply so
           an `--d1 ansi` mapping lands the host on a real ANSI driver
           instead of UNKTERM. The filter both swallows the query AND
           emits the VT100-shaped DA1 response. */
        {
            const term_id_entry_t *ansi = term_id_lookup("ANSI");
            static const uint8 in[] = { 0x1B, '[', 'c' };
            static const uint8 expected[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };

            term_id_filter_init(&t);
            n = term_id_filter_process(&t, ansi, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0,
                  "filter-9d: ANSI swallows the DA query");
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-9e: ANSI emits VT100-style DA1 response");
        }

        /* 10. CSI overflow: 12 parameter bytes ESC[1;2;3;4;5;6m. */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = {
                0x1B, '[',
                '1', ';', '2', ';', '3', ';', '4', ';',
                '5', ';', '6', ';', '7', ';', '8', 'm'
            };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == (uint32)sizeof(in) &&
                  bytes_eq(out, n, in, sizeof(in)),
                  "filter-10a: long SGR passes through via FLUSH_THROUGH");
            CHECK(resp_len == 0,
                  "filter-10b: long SGR produces no response");
        }

        /* 11. Two DA1 queries in one call: concatenated responses. */
        term_id_filter_init(&t);
        {
            static const uint8 in[] =
                { 0x1B, '[', 'c', 0x1B, '[', 'c' };
            static const uint8 one_da1[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, vt100, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0,
                  "filter-11a: two queries -> zero forwarded bytes");
            CHECK(resp_len == 2 * sizeof(one_da1),
                  "filter-11b: response length is two DA1 replies");
            CHECK(bytes_eq(resp, sizeof(one_da1),
                           one_da1, sizeof(one_da1)) &&
                  bytes_eq(resp + sizeof(one_da1), sizeof(one_da1),
                           one_da1, sizeof(one_da1)),
                  "filter-11c: both responses identical and intact");
        }

        /* 12. VT220 profile: DA1 emits VT220-specific reply. */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', 'c' };
            static const uint8 expected[] = {
                0x1B, '[', '?', '6', '2', ';',
                '1', ';', '2', ';', '6', ';', '8', ';', '9',
                ';', '1', '5', 'c'
            };
            n = term_id_filter_process(&t, vt220, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(n == 0,
                  "filter-12a: VT220 also swallows the query");
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-12b: response is VT220 DA1");
        }

        /* 13. NULL id: function uses term_id_default() (VT100). */
        term_id_filter_init(&t);
        {
            static const uint8 in[] = { 0x1B, '[', 'c' };
            static const uint8 expected[] =
                { 0x1B, '[', '?', '1', ';', '0', 'c' };
            n = term_id_filter_process(&t, NULL, in, sizeof(in),
                                       out, resp, sizeof(resp), &resp_len);
            CHECK(bytes_eq(resp, resp_len, expected, sizeof(expected)),
                  "filter-13: NULL id falls back to VT100 default");
        }
    }

    /* === (C) Telenet code resolver (v1.5.6) ==================== */
    {
        const char *r;

        /* D1 with no override resolves to VT100. */
        r = term_id_resolve_telenet_code("D1", NULL);
        CHECK(r != NULL && strcmp(r, "VT100") == 0,
              "telenet-D1 (no override): -> VT100");
        r = term_id_resolve_telenet_code("D1", "");
        CHECK(r != NULL && strcmp(r, "VT100") == 0,
              "telenet-D1 (empty override): -> VT100");
        r = term_id_resolve_telenet_code("d1", NULL);  /* case-insensitive */
        CHECK(r != NULL && strcmp(r, "VT100") == 0,
              "telenet-d1 (lc, no override): -> VT100");

        /* D1 with an override returns the override (operator's choice). */
        r = term_id_resolve_telenet_code("D1", "ANSI");
        CHECK(r != NULL && strcmp(r, "ANSI") == 0,
              "telenet-D1 (override ANSI): -> ANSI");
        r = term_id_resolve_telenet_code("D1", "DUMB");
        CHECK(r != NULL && strcmp(r, "DUMB") == 0,
              "telenet-D1 (override DUMB): -> DUMB");

        /* A-class and B-class codes always resolve to DUMB, ignoring
           the d1_override (those are hardcopy/teletype-class). */
        r = term_id_resolve_telenet_code("A1", NULL);
        CHECK(r != NULL && strcmp(r, "DUMB") == 0,
              "telenet-A1: -> DUMB");
        r = term_id_resolve_telenet_code("A9", "ANSI"); /* override ignored */
        CHECK(r != NULL && strcmp(r, "DUMB") == 0,
              "telenet-A9 (override ANSI): -> DUMB (A-class is unambiguous)");
        r = term_id_resolve_telenet_code("B3", NULL);
        CHECK(r != NULL && strcmp(r, "DUMB") == 0,
              "telenet-B3: -> DUMB");
        r = term_id_resolve_telenet_code("b5", NULL);
        CHECK(r != NULL && strcmp(r, "DUMB") == 0,
              "telenet-b5 (lc): -> DUMB");

        /* Names that aren't Telenet codes pass through verbatim. */
        r = term_id_resolve_telenet_code("VT100", NULL);
        CHECK(r != NULL && strcmp(r, "VT100") == 0,
              "telenet-VT100 (not a code): pass through");
        r = term_id_resolve_telenet_code("XTERM", "ANSI");
        CHECK(r != NULL && strcmp(r, "XTERM") == 0,
              "telenet-XTERM (not a code, override irrelevant): pass through");

        /* Two-char strings that LOOK like Telenet codes but use a
           digit outside 1-9, or a wrong prefix letter, fall through. */
        r = term_id_resolve_telenet_code("A0", NULL);
        CHECK(r != NULL && strcmp(r, "A0") == 0,
              "telenet-A0 (digit out of range): pass through");
        r = term_id_resolve_telenet_code("C1", NULL);
        CHECK(r != NULL && strcmp(r, "C1") == 0,
              "telenet-C1 (wrong class letter): pass through");
        r = term_id_resolve_telenet_code("D2", NULL);
        CHECK(r != NULL && strcmp(r, "D2") == 0,
              "telenet-D2 (only D1 is recognised): pass through");

        /* NULL input -> NULL output. */
        CHECK(term_id_resolve_telenet_code(NULL, NULL) == NULL,
              "telenet-NULL: returns NULL");
    }

    if (g_fail) {
        fprintf(stderr, "test_term_id: FAILED\n");
        return 1;
    }
    fprintf(stderr, "test_term_id: all scenarios pass\n");
    return 0;
}
