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

/* Tests for X.28 command parsing and service signal formatting.
   Examples cited from X.28 (12/97) clause 3.5. */
#include "test.h"
#include "x28_signals.h"
#include "x3.h"

static int parse(const char *s, x28_command_t *out)
{
    return x28_parse_command(s, (uint32)strlen(s), NULL, out);
}

static int parse_with_aliases(const char *s,
                              const x28_command_alias_t *aliases,
                              x28_command_t *out)
{
    return x28_parse_command(s, (uint32)strlen(s), aliases, out);
}

/* ---- command parsing -------------------------------------------------- */

static void test_parse_par_q(void)
{
    x28_command_t cmd;
    /* X.28 §3.5.4 example: "PAR? 1, 3, 5" */
    ASSERT_EQ_INT(parse("PAR? 1, 3, 5", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);
    ASSERT_EQ_INT(cmd.param_count, 3);
    ASSERT_EQ_INT(cmd.params[0].ref, 1);
    ASSERT_EQ_INT(cmd.params[1].ref, 3);
    ASSERT_EQ_INT(cmd.params[2].ref, 5);

    /* Empty list = "all" (§3.5.4): "If no parameter reference number is
       indicated... then it applies implicitly to all parameters." */
    ASSERT_EQ_INT(parse("PAR?", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);
    ASSERT_EQ_INT(cmd.param_count, 0);

    /* Trailing CR delimiter allowed and stripped. */
    ASSERT_EQ_INT(parse("PAR?\r", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);

    /* Lower case acceptable per §3.5. */
    ASSERT_EQ_INT(parse("par? 2", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);
    ASSERT_EQ_INT(cmd.params[0].ref, 2);
}

static void test_parse_set(void)
{
    x28_command_t cmd;
    /* X.28 §3.5.6 example: "SET 2:0, 3:2, 9:4" */
    ASSERT_EQ_INT(parse("SET 2:0, 3:2, 9:4", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SET);
    ASSERT_EQ_INT(cmd.param_count, 3);
    ASSERT_EQ_INT(cmd.params[0].ref, 2); ASSERT_EQ_INT(cmd.params[0].value, 0);
    ASSERT_EQ_INT(cmd.params[1].ref, 3); ASSERT_EQ_INT(cmd.params[1].value, 2);
    ASSERT_EQ_INT(cmd.params[2].ref, 9); ASSERT_EQ_INT(cmd.params[2].value, 4);
}

static void test_parse_set_read(void)
{
    x28_command_t cmd;
    /* §3.5.6: set-and-read uses "?" after SET. */
    ASSERT_EQ_INT(parse("SET? 2:1", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SET_READ);
    ASSERT_EQ_INT(cmd.params[0].ref, 2);
    ASSERT_EQ_INT(cmd.params[0].value, 1);
}

static void test_parse_set_accepts_any_value(void)
{
    x28_command_t cmd;
    /* Parser does not value-validate; the dispatcher is responsible for
       per-X.3 range checks and "INV" reporting per X.28 3.5.14. */
    ASSERT_EQ_INT(parse("SET 1:5", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SET);
    ASSERT_EQ_INT(cmd.params[0].ref, 1);
    ASSERT_EQ_INT(cmd.params[0].value, 5);
    ASSERT_EQ_INT(cmd.params[0].invalid, 0);
}

static void test_parse_set_rejects_bad_ref(void)
{
    x28_command_t cmd;
    /* Refs 1..30 are accepted (extended set 23..30 added 2026-05-23);
       0 and >30 still reject. */
    ASSERT_EQ_INT(parse("SET 0:0",   &cmd), X28_PARSE_ERR_BAD_REF);
    ASSERT_EQ_INT(parse("SET 31:0",  &cmd), X28_PARSE_ERR_BAD_REF);
    ASSERT_EQ_INT(parse("SET 255:0", &cmd), X28_PARSE_ERR_BAD_REF);
}

static void test_parse_prof(void)
{
    x28_command_t cmd;
    /* §3.5.5 + Table 3/X.28: profile 90 = simple, 91 = transparent. */
    ASSERT_EQ_INT(parse("PROF 90", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PROF);
    ASSERT_EQ_INT(cmd.profile_id, 90);

    ASSERT_EQ_INT(parse("PROF", &cmd), X28_PARSE_ERR_SYNTAX);
}

static void test_parse_simple_commands(void)
{
    x28_command_t cmd;
    /* §3.5.10 STAT, §3.5.12 RESET, §3.5.13 INT, §3.5.8 CLR, §3.5.8.2 ICLR. */
    ASSERT_EQ_INT(parse("STAT",  &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_STAT);

    ASSERT_EQ_INT(parse("RESET", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_RESET);

    ASSERT_EQ_INT(parse("INT",   &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_INT);

    ASSERT_EQ_INT(parse("CLR",   &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);

    ASSERT_EQ_INT(parse("ICLR",  &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_ICLR);
}

static void test_parse_remote(void)
{
    x28_command_t cmd;
    /* §3.5.4.2 example: "RPAR? 1, 3, 5". */
    ASSERT_EQ_INT(parse("RPAR? 1, 3, 5", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_RPAR);
    ASSERT_EQ_INT(cmd.param_count, 3);

    /* §3.5.6.2 example: "RSET 2:0, 3:2, 9:4". */
    ASSERT_EQ_INT(parse("RSET 2:0, 3:2", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_RSET);
}

static void test_parse_selection(void)
{
    x28_command_t cmd;
    /* §3.5.15: address block of digits is a selection PAD command signal. */
    ASSERT_EQ_INT(parse("12345678", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.address_len, 8);
    ASSERT_TRUE(strcmp(cmd.address, "12345678") == 0);
    /* No facility block. */
    ASSERT_EQ_INT(cmd.facility_count, 0);
}

static void test_parse_selection_single_facility(void)
{
    x28_command_t cmd;
    /* §3.5.15.1.3: <R> = reverse charging request (no argument). */
    ASSERT_EQ_INT(parse("R-12345", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.facility_count, 1);
    ASSERT_EQ_INT(cmd.facilities[0].code, 'R');
    ASSERT_EQ_INT(cmd.facilities[0].arg_len, 0);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
}

static void test_parse_selection_facility_with_arg(void)
{
    x28_command_t cmd;
    /* §3.5.15.1.1: <N> <NUI string>. The NUI string is everything from
       after the 'N' up to the facility separator or terminator. */
    ASSERT_EQ_INT(parse("Nalice-12345", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.facility_count, 1);
    ASSERT_EQ_INT(cmd.facilities[0].code, 'N');
    ASSERT_TRUE(strcmp(cmd.facilities[0].arg, "alice") == 0);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
}

static void test_parse_selection_multiple_facilities(void)
{
    x28_command_t cmd;
    /* §3.5.15.1: ',' separates facility codes. */
    ASSERT_EQ_INT(parse("G05,R-67890", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.facility_count, 2);
    ASSERT_EQ_INT(cmd.facilities[0].code, 'G');
    ASSERT_TRUE(strcmp(cmd.facilities[0].arg, "05") == 0);
    ASSERT_EQ_INT(cmd.facilities[1].code, 'R');
    ASSERT_EQ_INT(cmd.facilities[1].arg_len, 0);
    ASSERT_TRUE(strcmp(cmd.address, "67890") == 0);
}

static void test_parse_selection_dash_in_pure_address(void)
{
    x28_command_t cmd;
    /* A '-' in input that ISN'T preceded by a facility letter is part
       of the address. (No real X.121 address contains '-', but the
       parser must not mistakenly slice on it.) */
    ASSERT_EQ_INT(parse("12345-67890", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.facility_count, 0);
    ASSERT_TRUE(strcmp(cmd.address, "12345-67890") == 0);
}

static void test_parse_selection_cud_d_prefix(void)
{
    x28_command_t cmd;
    /* §3.5.15.3: D prefix = IA5 character call user data. */
    ASSERT_EQ_INT(parse("12345DHELLO", &cmd), X28_PARSE_OK);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
    ASSERT_EQ_INT(cmd.cud_type, 'D');
    ASSERT_TRUE(strcmp(cmd.cud_data, "HELLO") == 0);
    ASSERT_EQ_INT(cmd.cud_len, 5);
}

static void test_parse_selection_cud_p_with_space(void)
{
    x28_command_t cmd;
    /* §3.5.15.3: P prefix = printable IA5 subset. Leading whitespace
       after the prefix is stripped. */
    ASSERT_EQ_INT(parse("12345 P  THIS IS USER DATA", &cmd), X28_PARSE_OK);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
    ASSERT_EQ_INT(cmd.cud_type, 'P');
    ASSERT_TRUE(strcmp(cmd.cud_data, "THIS IS USER DATA") == 0);
}

static void test_parse_selection_cud_h_hex(void)
{
    x28_command_t cmd;
    /* §3.5.15.3: H prefix = hex pairs representing binary. Parser
       does not decode; payload is captured verbatim. */
    ASSERT_EQ_INT(parse("12345H48656C6C6F", &cmd), X28_PARSE_OK);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
    ASSERT_EQ_INT(cmd.cud_type, 'H');
    ASSERT_TRUE(strcmp(cmd.cud_data, "48656C6C6F") == 0);
}

static void test_parse_selection_cud_with_facility_block(void)
{
    x28_command_t cmd;
    /* Facility block + address + CUD all in one selection signal. */
    ASSERT_EQ_INT(parse("Ndavid,R-30001DLOGIN", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.facility_count, 2);
    ASSERT_EQ_INT(cmd.facilities[0].code, 'N');
    ASSERT_TRUE(strcmp(cmd.facilities[0].arg, "david") == 0);
    ASSERT_EQ_INT(cmd.facilities[1].code, 'R');
    ASSERT_TRUE(strcmp(cmd.address, "30001") == 0);
    ASSERT_EQ_INT(cmd.cud_type, 'D');
    ASSERT_TRUE(strcmp(cmd.cud_data, "LOGIN") == 0);
}

static void test_parse_selection_no_cud_no_prefix_seen(void)
{
    x28_command_t cmd;
    /* Pure-digit address has no CUD; cud_type stays 0. */
    ASSERT_EQ_INT(parse("12345", &cmd), X28_PARSE_OK);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
    ASSERT_EQ_INT(cmd.cud_type, 0);
    ASSERT_EQ_INT(cmd.cud_len, 0);
}

static void test_parse_selection_lone_prefix_letter_not_cud(void)
{
    x28_command_t cmd;
    /* A bare D/P/H with nothing after it isn't a CUD start - the
       parser only recognises CUD when at least one payload byte follows. */
    ASSERT_EQ_INT(parse("12345D", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.cud_type, 0);
    ASSERT_EQ_INT(cmd.cud_len, 0);
}

static void test_parse_selection_via_call_keyword(void)
{
    x28_command_t cmd;
    /* The 'CALL' extended-dialogue keyword goes through the same
       facility/address parsing. */
    ASSERT_EQ_INT(parse("CALL N42,R-12345", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.facility_count, 2);
    ASSERT_EQ_INT(cmd.facilities[0].code, 'N');
    ASSERT_TRUE(strcmp(cmd.facilities[0].arg, "42") == 0);
    ASSERT_EQ_INT(cmd.facilities[1].code, 'R');
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
}

static void test_parse_empty_and_garbage(void)
{
    x28_command_t cmd;
    ASSERT_EQ_INT(parse("",   &cmd), X28_PARSE_ERR_EMPTY);
    ASSERT_EQ_INT(parse("  ", &cmd), X28_PARSE_ERR_EMPTY);

    /* SET without any pair is malformed. */
    ASSERT_EQ_INT(parse("SET", &cmd), X28_PARSE_ERR_SYNTAX);

    /* SET p without :value */
    ASSERT_EQ_INT(parse("SET 2", &cmd), X28_PARSE_ERR_SYNTAX);
}

/* ---- service signal formatting --------------------------------------- */

static void test_format_ack(void)
{
    uint8 buf[16];
    int32 n = x28_format_ack(buf, sizeof(buf));
    /* §3.5.3: just the format effector = CR LF. */
    ASSERT_EQ_INT(n, 2);
    ASSERT_EQ_INT(buf[0], 0x0D);
    ASSERT_EQ_INT(buf[1], 0x0A);
}

static void test_format_par(void)
{
    uint8 buf[64];
    x28_param_pair_t params[2];
    int32 n;
    /* §3.5: leading + trailing format effector for non-exception signals. */
    const char expected[] = "\r\nPAR 2:1, 3:126\r\n";

    memset(params, 0, sizeof(params));
    params[0].ref = 2; params[0].value = 1;
    params[1].ref = 3; params[1].value = 126;
    n = x28_format_par(params, 2, buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

static void test_format_par_with_invalid(void)
{
    uint8 buf[64];
    x28_param_pair_t params[2];
    int32 n;
    /* X.28 3.5.14: "INV" in place of value for invalid pair. §3.5: leading
       + trailing effector. */
    const char expected[] = "\r\nPAR 1:INV, 2:0\r\n";

    memset(params, 0, sizeof(params));
    params[0].ref = 1; params[0].value = 5; params[0].invalid = 1;
    params[1].ref = 2; params[1].value = 0; params[1].invalid = 0;
    n = x28_format_par(params, 2, buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

static void test_format_clr_confirmation(void)
{
    uint8 buf[32];
    int32 n;
    const char expected[] = "\r\nCLR CONF\r\n";
    n = x28_format_clr_confirmation(buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

static void test_format_connected(void)
{
    uint8 buf[16];
    int32 n;
    const char expected[] = "\r\nCOM\r\n";
    /* §3.5.21: connected PAD service signal minimal form. */
    n = x28_format_connected(buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

static void test_format_err(void)
{
    uint8 buf[16];
    int32 n = x28_format_err(buf, sizeof(buf));
    const char expected[] = "\r\nERR\r\n";
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

static void test_format_status(void)
{
    uint8 buf[32];
    const char eng[] = "\r\nENGAGED\r\n";
    const char free_[] = "\r\nFREE\r\n";
    int32 n;

    n = x28_format_status_engaged(buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(eng) - 1);
    ASSERT_MEM_EQ(buf, eng, sizeof(eng) - 1);

    n = x28_format_status_free(buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(free_) - 1);
    ASSERT_MEM_EQ(buf, free_, sizeof(free_) - 1);
}

static void test_format_clr_indication(void)
{
    uint8 buf[64];
    const char expected[] = "\r\nCLR DTE C:0 D:0\r\n";
    int32 n;
    /* §3.5.17.1 + Table 6/X.28: "DTE" = call cleared, remote request. */
    n = x28_format_clr_indication("DTE", 0, 0, buf, sizeof(buf));
    ASSERT_EQ_INT(n, (int32)sizeof(expected) - 1);
    ASSERT_MEM_EQ(buf, expected, sizeof(expected) - 1);
}

/* ---- extended dialogue mode (X.28 §5) -------------------------------- */

static void test_parse_extended_break(void)
{
    x28_command_t cmd;
    ASSERT_EQ_INT(parse("BREAK", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_BREAK);
}

static void test_parse_extended_nui(void)
{
    x28_command_t cmd;
    /* §5.2: NUI ON = "ID" + optional NUI string; NUI OFF = "IDOFF". */
    ASSERT_EQ_INT(parse("ID my-nui", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_NUI_ON);
    ASSERT_TRUE(strcmp(cmd.address, "my-nui") == 0);

    /* "ID" alone: NUI string is empty. */
    ASSERT_EQ_INT(parse("ID", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_NUI_ON);
    ASSERT_EQ_INT(cmd.address_len, 0);

    /* "IDOFF" must not be confused with "ID OFF". */
    ASSERT_EQ_INT(parse("IDOFF", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_NUI_OFF);
}

static void test_parse_extended_lang(void)
{
    x28_command_t cmd;
    /* §5.3: LANG or LANGUAGE + language string. */
    ASSERT_EQ_INT(parse("LANG ENGLISH", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_LANG);
    ASSERT_TRUE(strcmp(cmd.address, "ENGLISH") == 0);

    ASSERT_EQ_INT(parse("LANGUAGE FRENCH", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_LANG);
    ASSERT_TRUE(strcmp(cmd.address, "FRENCH") == 0);
}

static void test_parse_extended_help(void)
{
    x28_command_t cmd;
    /* §5.4: HELP + optional subject. */
    ASSERT_EQ_INT(parse("HELP", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_HELP);
    ASSERT_EQ_INT(cmd.address_len, 0);

    ASSERT_EQ_INT(parse("HELP PARAMETER 1", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_HELP);
    ASSERT_TRUE(strcmp(cmd.address, "PARAMETER 1") == 0);
}

static void test_parse_extended_aliases(void)
{
    x28_command_t cmd;
    /* Table 9/X.28: each extended keyword maps to its standard form. */
    ASSERT_EQ_INT(parse("STATUS",    &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_STAT);
    ASSERT_EQ_INT(parse("CLEAR",     &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);
    ASSERT_EQ_INT(parse("INTERRUPT", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_INT);
    ASSERT_EQ_INT(parse("PROFILE 90",&cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PROF);
    ASSERT_EQ_INT(cmd.profile_id, 90);
    ASSERT_EQ_INT(parse("PARAMETER 2", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);
    ASSERT_EQ_INT(cmd.params[0].ref, 2);
    ASSERT_EQ_INT(parse("READ 2",    &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_PAR);
    ASSERT_EQ_INT(parse("SETREAD 2:0", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SET_READ);
    ASSERT_EQ_INT(parse("RREAD 1",   &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_RPAR);
    ASSERT_EQ_INT(parse("RSETREAD 1:1", &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_RSET);
    ASSERT_EQ_INT(parse("ICLEAR",    &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_ICLR);
    ASSERT_EQ_INT(parse("CALL 12345",&cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);
}

static void test_personality_aliases_mechanism(void)
{
    /* Parser-level test of the personality command-alias hook. Uses a
       throwaway alias table to exercise match rules independently of
       any specific personality. Aliases follow the same terminator
       rule as built-in keywords: the byte after the keyword must be
       non-alphanumeric (or end-of-input). */
    static const x28_command_alias_t aliases[] = {
        { "CONNECT",    X28_CMD_SELECTION, 1, NULL, 0 },  /* takes_address */
        { "C",          X28_CMD_SELECTION, 1, NULL, 0 },  /* takes_address */
        { "DISCONNECT", X28_CMD_CLR,       0, NULL, 0 },  /* bare */
        { "D",          X28_CMD_CLR,       0, NULL, 0 },  /* bare */
        { NULL,         0,                 0, NULL, 0 }
    };
    x28_command_t cmd;

    /* Whitespace-separated form: keyword followed by an address. */
    ASSERT_EQ_INT(parse_with_aliases("c 12345", aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);

    ASSERT_EQ_INT(parse_with_aliases("CONNECT 12345", aliases, &cmd),
                  X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);

    /* No space => no alias match. "C12345" must NOT be parsed as
       alias C + address 12345; it falls through to bare SELECTION
       (whose address scanner stops at 'C', leaving an empty address). */
    ASSERT_EQ_INT(parse_with_aliases("C12345", aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.address_len, 0);

    /* Same for the longer keyword: "CONNECT12345" must NOT match. */
    ASSERT_EQ_INT(parse_with_aliases("CONNECT12345", aliases, &cmd),
                  X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.address_len, 0);

    /* Longest-match: "CONNECT 12345" is matched as the full keyword,
       not as alias "C" followed by garbage "ONNECT 12345". */
    ASSERT_EQ_INT(parse_with_aliases("CONNECT 12345", aliases, &cmd),
                  X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_TRUE(strcmp(cmd.address, "12345") == 0);

    /* Bare aliases (takes_address=0) work alone and dispatch to CLR. */
    ASSERT_EQ_INT(parse_with_aliases("D",          aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);
    ASSERT_EQ_INT(parse_with_aliases("DISCONNECT", aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);
    ASSERT_EQ_INT(parse_with_aliases("d",          aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);

    /* Bare alias rejects an alphanumeric suffix: "D12345" and
       "DISCONNECTOR" must NOT match -- they fall through to SELECTION. */
    ASSERT_EQ_INT(parse_with_aliases("D12345", aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(parse_with_aliases("DISCONNECTOR", aliases, &cmd),
                  X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);

    /* Standard X.28 keywords still win over aliases: a "CLR" command
       parses as X28_CMD_CLR via the built-in keyword, not via alias D
       or DISCONNECT. */
    ASSERT_EQ_INT(parse_with_aliases("CLR", aliases, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_CLR);

    /* Passing NULL aliases disables the feature entirely. */
    ASSERT_EQ_INT(parse_with_aliases("c 12345", NULL, &cmd), X28_PARSE_OK);
    ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    ASSERT_EQ_INT(cmd.address_len, 0);

    /* CONTINUE / CONT alias support (Telenet "return to data mode"
       keyword). Aliased to X28_CMD_CONTINUE, takes no argument.
       Verify both forms recognise and that the short form does not
       false-match against the longer one (terminator rule). */
    {
        static const x28_command_alias_t cont_aliases[] = {
            { "CONTINUE", X28_CMD_CONTINUE, 0, NULL, 0 },
            { "CONT",     X28_CMD_CONTINUE, 0, NULL, 0 },
            { NULL,       0,                0, NULL, 0 }
        };
        ASSERT_EQ_INT(parse_with_aliases("CONT", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_CONTINUE);
        ASSERT_EQ_INT(parse_with_aliases("cont", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_CONTINUE);
        ASSERT_EQ_INT(parse_with_aliases("CONTINUE", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_CONTINUE);
        ASSERT_EQ_INT(parse_with_aliases("continue", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_CONTINUE);
        /* "CONTINUE" must match the long alias, NOT the short one
           that happens to be a prefix; the terminator rule on CONT
           rejects the 'I' that follows it. */
        ASSERT_EQ_INT(parse_with_aliases("CONTINUE", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_CONTINUE);
        /* Bare alias rejects alphanumeric suffix. */
        ASSERT_EQ_INT(parse_with_aliases("CONTROL", cont_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_SELECTION);
    }

    /* Preset-pairs mechanism (Telenet "HALF"/"FULL"). The alias maps
       to X28_CMD_SET and carries inline ref:val args that the parser
       pre-populates into out->params before returning. */
    {
        static const uint8 half_pairs[] = { 2, 0 };
        static const uint8 full_pairs[] = { 2, 1 };
        static const x28_command_alias_t set_aliases[] = {
            { "HALF", X28_CMD_SET, 0, half_pairs, 1 },
            { "FULL", X28_CMD_SET, 0, full_pairs, 1 },
            { NULL,   0,           0, NULL,       0 }
        };
        ASSERT_EQ_INT(parse_with_aliases("HALF", set_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_SET);
        ASSERT_EQ_INT(cmd.param_count, 1);
        ASSERT_EQ_INT(cmd.params[0].ref, 2);
        ASSERT_EQ_INT(cmd.params[0].value, 0);
        ASSERT_EQ_INT(cmd.params[0].invalid, 0);

        ASSERT_EQ_INT(parse_with_aliases("full", set_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_SET);
        ASSERT_EQ_INT(cmd.param_count, 1);
        ASSERT_EQ_INT(cmd.params[0].ref, 2);
        ASSERT_EQ_INT(cmd.params[0].value, 1);
    }

    /* Multi-pair preset: a single alias may carry several ref:val
       pairs in one shot. */
    {
        static const uint8 multi_pairs[] = { 2, 0, 3, 0, 4, 20 };
        static const x28_command_alias_t multi_aliases[] = {
            { "TRANSPARENT", X28_CMD_SET, 0, multi_pairs, 3 },
            { NULL,          0,           0, NULL,        0 }
        };
        ASSERT_EQ_INT(parse_with_aliases("TRANSPARENT", multi_aliases, &cmd),
                      X28_PARSE_OK);
        ASSERT_EQ_INT(cmd.type, X28_CMD_SET);
        ASSERT_EQ_INT(cmd.param_count, 3);
        ASSERT_EQ_INT(cmd.params[0].ref, 2);
        ASSERT_EQ_INT(cmd.params[0].value, 0);
        ASSERT_EQ_INT(cmd.params[1].ref, 3);
        ASSERT_EQ_INT(cmd.params[1].value, 0);
        ASSERT_EQ_INT(cmd.params[2].ref, 4);
        ASSERT_EQ_INT(cmd.params[2].value, 20);
    }
}

static void test_format_help(void)
{
    uint8 buf[256];
    int32 n = x28_format_help("", buf, sizeof(buf));
    /* §3.5: leading effector. Empty subject emits the command list. */
    ASSERT_TRUE(n > 0);
    ASSERT_EQ_INT(buf[0], 0x0D);
    ASSERT_EQ_INT(buf[1], 0x0A);
    ASSERT_EQ_INT(buf[2], 'H');
    ASSERT_EQ_INT(buf[n - 2], 0x0D);
    ASSERT_EQ_INT(buf[n - 1], 0x0A);
}

static int help_body_contains(const char *subject, const char *needle)
{
    uint8 buf[256];
    uint32 i, nlen;
    int32 n = x28_format_help(subject, buf, sizeof(buf));
    if (n <= 0) return 0;
    nlen = (uint32)strlen(needle);
    if (nlen > (uint32)n) return 0;
    for (i = 0; (int32)(i + nlen) <= n; i++) {
        if (memcmp(buf + i, needle, nlen) == 0) return 1;
    }
    return 0;
}

static void test_format_help_subject_par(void)
{
    /* Table 10/X.28: HELP PAR explains the parameter-read command. */
    ASSERT_TRUE(help_body_contains("PAR", "PAR?"));
    ASSERT_TRUE(help_body_contains("PARAMETER", "PAR?"));
}

static void test_format_help_subject_set(void)
{
    ASSERT_TRUE(help_body_contains("SET", "SET"));
    ASSERT_TRUE(help_body_contains("set", "SET"));    /* case-insensitive */
}

static void test_format_help_subject_id(void)
{
    /* Distinct text for ID vs IDOFF. */
    ASSERT_TRUE(help_body_contains("ID", "session-level"));
    ASSERT_TRUE(help_body_contains("IDOFF", "clear"));
}

static void test_format_help_unknown_subject(void)
{
    ASSERT_TRUE(help_body_contains("FROBNITZ", "not recognised"));
}

static void test_format_buffer_overflow(void)
{
    uint8 small[3];
    int32 n;
    /* "ENGAGED\r\n" is 9 bytes, buffer too small => negative. */
    n = x28_format_status_engaged(small, sizeof(small));
    ASSERT_TRUE(n < 0);
}

int main(void)
{
    test_parse_par_q();
    test_parse_set();
    test_parse_set_read();
    test_parse_set_accepts_any_value();
    test_parse_set_rejects_bad_ref();
    test_parse_prof();
    test_parse_simple_commands();
    test_parse_remote();
    test_parse_selection();
    test_parse_selection_single_facility();
    test_parse_selection_facility_with_arg();
    test_parse_selection_multiple_facilities();
    test_parse_selection_dash_in_pure_address();
    test_parse_selection_cud_d_prefix();
    test_parse_selection_cud_p_with_space();
    test_parse_selection_cud_h_hex();
    test_parse_selection_cud_with_facility_block();
    test_parse_selection_no_cud_no_prefix_seen();
    test_parse_selection_lone_prefix_letter_not_cud();
    test_parse_selection_via_call_keyword();
    test_parse_empty_and_garbage();
    test_format_ack();
    test_format_par();
    test_format_par_with_invalid();
    test_format_clr_confirmation();
    test_format_connected();
    test_format_err();
    test_format_status();
    test_format_clr_indication();
    test_parse_extended_break();
    test_parse_extended_nui();
    test_parse_extended_lang();
    test_parse_extended_help();
    test_parse_extended_aliases();
    test_personality_aliases_mechanism();
    test_format_help();
    test_format_help_subject_par();
    test_format_help_subject_set();
    test_format_help_subject_id();
    test_format_help_unknown_subject();
    test_format_buffer_overflow();
    TEST_REPORT();
}
