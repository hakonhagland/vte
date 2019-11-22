/*
 * Copyright © 2015 Egmont Koblinger
 * Copyright © 2019 Christian Persch
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "config.h"

#include <glib.h>
#include <locale.h>

#include <cstdint>
#include <cstdlib>

#include <string>

#include "regex.hh"
#include "regex-builtins-patterns.hh"

auto pcre2test = bool{false};
auto pcre2_atleast_10_34 = bool{false};

/* Shorthand for expecting the pattern to match the entire input string */
#define ENTIRE ((char *) 1)

static pcre2_match_context_8*
create_match_context()
{
        pcre2_match_context_8 *match_context;

        match_context = pcre2_match_context_create_8(nullptr /* general context */);
        pcre2_set_match_limit_8(match_context, 65536); /* should be plenty */
        pcre2_set_recursion_limit_8(match_context, 64); /* should be plenty */

        return match_context;
}

static char*
get_match(decltype(&pcre2_match_8) match_fn,
          vte::base::Regex const* regex,
          uint32_t match_flags,
          char const* subject)
{
        auto match_context = create_match_context();
        auto match_data = pcre2_match_data_create_8(256 /* should be plenty */,
                                                    nullptr /* general context */);

        auto r = match_fn(regex->code(),
                          (PCRE2_SPTR8)subject,
                          strlen(subject),
                          0, /* start offset */
                          match_flags |
                          PCRE2_NO_UTF_CHECK,
                          match_data,
                          match_context);

        char* match;
        if (r == PCRE2_ERROR_NOMATCH) {
                match = nullptr;
        } else if (r < 0) {
                /* Error */
                PCRE2_UCHAR8 buf[256];
                int n = pcre2_get_error_message_8(r, buf, sizeof(buf));
                g_assert_true(n >= 0);
                g_printerr("PCRE2 error %d: %s\n", r, buf);

                match = nullptr;
        } else {
                /* has match */
                auto const* ovector = pcre2_get_ovector_pointer_8(match_data);
                auto const so = ovector[0];
                auto const eo = ovector[1];
                if (so == PCRE2_UNSET || eo == PCRE2_UNSET)
                        match = nullptr;
                else
                        match = g_strndup(subject + so, eo - so);
        }

        pcre2_match_data_free_8(match_data);
        pcre2_match_context_free_8(match_context);

        return match;
}

struct TestData {
        char const* pattern;
        char const* string;
        char const* expected;
        uint32_t match_flags;
};

static std::string
escape_slash(std::string str)
{
        std::string escaped;
        for (auto const c : str) {
                if (c == '/')
                        escaped.append("\\/");
                else if (c == '\\')
                        escaped.append("\\\\");
                else
                        escaped.push_back(c);
        }

        return escaped;
}

static std::string
flags_to_string(uint32_t flags)
{
        std::string str;

        if (flags & PCRE2_ANCHORED)
                str.append("anchored,");

        return str;
}

static void
print_testdata(TestData* data,
               int line)
{
        auto patstr = escape_slash(data->pattern);
        auto flagstr = flags_to_string(data->match_flags);

        g_print("# Line: %d\n"
                "# Expected match: '%s'\n"
                "/%s/%s\n"
                "    %s\\=\n"
                "\n",
                line,
                data->expected ? data->expected : "No match",
                patstr.c_str(), flagstr.c_str(),
                data->string);
}

static void
assert_match_test(void const* ptr)
{
        TestData const* data = (TestData*)ptr;

        GError *error{nullptr};
        auto regex = vte::base::Regex::compile(vte::base::Regex::Purpose::eMatch,
                                               data->pattern,
                                               PCRE2_ZERO_TERMINATED,
                                               PCRE2_UTF | PCRE2_NO_UTF_CHECK |
                                               PCRE2_UCP |
                                               PCRE2_MULTILINE |
                                               /* Pass match_flags here as compile flags, since
                                                * otherwise some JITed regex tests fail because
                                                * ANCHORED is ignored when passed to
                                                * pcre2_jit_match_8.
                                                */
                                               data->match_flags,
                                               &error);
        g_assert_no_error(error);
        g_assert_nonnull(regex);

        auto match = get_match(&pcre2_match_8, regex, data->match_flags, data->string);

        g_assert_cmpstr(match, ==, data->expected);
        g_free(match);

        if (vte::base::Regex::check_pcre_config_jit()) {
                regex->jit(PCRE2_JIT_COMPLETE, &error);
                g_assert_no_error(error);
                regex->jit(PCRE2_JIT_PARTIAL_SOFT, &error);
                g_assert_no_error(error);
                regex->jit(PCRE2_JIT_PARTIAL_HARD, &error);
                g_assert_no_error(error);

                match = get_match(&pcre2_jit_match_8, regex, data->match_flags, data->string);
                g_assert_cmpstr(match, ==, data->expected);
                g_free(match);
        }

        regex->unref();
}

static void
assert_match(char const* pattern,
             char const* string,
             char const* expected,
             uint32_t match_flags = 0u,
             int line = __builtin_LINE())
{
        TestData* data = g_new(TestData, 1);
        data->pattern = pattern;
        data->string = string;
        data->expected = expected == ENTIRE ? string : expected;
        data->match_flags = match_flags;

        char* path = g_strdup_printf("/vte/regex/builtins/%d", line);
        g_test_add_data_func_full(path, data, assert_match_test, (GDestroyNotify)g_free);
        g_free(path);

        if (pcre2test)
                print_testdata(data, line);
}

static void
assert_match_anchored(char const* pattern,
                      char const* string,
                      char const* expected,
                      int line = __builtin_LINE())
{
        assert_match(pattern, string, expected, PCRE2_ANCHORED, line);
}

static void
setup_regex_builtins_tests(void)
{
  /* SCHEME is case insensitive */
  assert_match_anchored (SCHEME, "http",  ENTIRE);
  assert_match_anchored (SCHEME, "HTTPS", ENTIRE);

  /* USER is nonempty, alphanumeric, dot, plus and dash */
  assert_match_anchored (USER, "",              NULL);
  assert_match_anchored (USER, "dr.john-smith", ENTIRE);
  assert_match_anchored (USER, "abc+def@ghi",   "abc+def");

  /* PASS is optional colon-prefixed value, allowing quite some characters, but definitely not @ */
  assert_match_anchored (PASS, "",          ENTIRE);
  assert_match_anchored (PASS, "nocolon",   "");
  assert_match_anchored (PASS, ":s3cr3T",   ENTIRE);
  assert_match_anchored (PASS, ":$?#@host", ":$?#");

  /* Hostname of at least 1 component, containing at least one non-digit in at least one of the segments */
  assert_match_anchored (HOSTNAME1, "example.com",       ENTIRE);
  assert_match_anchored (HOSTNAME1, "a-b.c-d",           ENTIRE);
  assert_match_anchored (HOSTNAME1, "a_b",               "a");    /* TODO: can/should we totally abort here? */
  assert_match_anchored (HOSTNAME1, "déjà-vu.com",       ENTIRE);
  assert_match_anchored (HOSTNAME1, "➡.ws",              ENTIRE);
  assert_match_anchored (HOSTNAME1, "cömbining-áccents", ENTIRE);
  assert_match_anchored (HOSTNAME1, "12",                NULL);
  assert_match_anchored (HOSTNAME1, "12.34",             NULL);
  assert_match_anchored (HOSTNAME1, "12.ab",             ENTIRE);
//  assert_match_anchored (HOSTNAME1, "ab.12",             NULL);  /* errr... could we fail here?? */

  /* Hostname of at least 2 components, containing at least one non-digit in at least one of the segments */
  assert_match_anchored (HOSTNAME2, "example.com",       ENTIRE);
  assert_match_anchored (HOSTNAME2, "example",           NULL);
  assert_match_anchored (HOSTNAME2, "12",                NULL);
  assert_match_anchored (HOSTNAME2, "12.34",             NULL);
  assert_match_anchored (HOSTNAME2, "12.ab",             ENTIRE);
  assert_match_anchored (HOSTNAME2, "ab.12",             NULL);
//  assert_match_anchored (HOSTNAME2, "ab.cd.12",          NULL);  /* errr... could we fail here?? */

  /* IPv4 segment (number between 0 and 255) */
  assert_match_anchored (DEFS "(?&S4)", "0",    ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "1",    ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "9",    ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "10",   ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "99",   ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "100",  ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "200",  ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "250",  ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "255",  ENTIRE);
  assert_match_anchored (DEFS "(?&S4)", "256",  NULL);
  assert_match_anchored (DEFS "(?&S4)", "260",  NULL);
  assert_match_anchored (DEFS "(?&S4)", "300",  NULL);
  assert_match_anchored (DEFS "(?&S4)", "1000", NULL);
  assert_match_anchored (DEFS "(?&S4)", "",     NULL);
  assert_match_anchored (DEFS "(?&S4)", "a1b",  NULL);

  /* IPv4 addresses */
  assert_match_anchored (DEFS "(?&IPV4)", "11.22.33.44",    ENTIRE);
  assert_match_anchored (DEFS "(?&IPV4)", "0.1.254.255",    ENTIRE);
  assert_match_anchored (DEFS "(?&IPV4)", "75.150.225.300", NULL);
  assert_match_anchored (DEFS "(?&IPV4)", "1.2.3.4.5",      "1.2.3.4");  /* we could also bail out and not match at all */

  /* IPv6 addresses */
  assert_match_anchored (DEFS "(?&IPV6)", "11:::22",                           NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22::33:44::55:66",               NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "dead::beef",                        ENTIRE);
  assert_match_anchored (DEFS "(?&IPV6)", "faded::bee",                        NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "live::pork",                        NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "::1",                               ENTIRE);
  assert_match_anchored (DEFS "(?&IPV6)", "11::22:33::44",                     NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:::33",                        NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "dead:beef::192.168.1.1",            ENTIRE);
  assert_match_anchored (DEFS "(?&IPV6)", "192.168.1.1",                       NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77:87654",        NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22::33:45678",                   NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:192.168.1.12345", NULL);

  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77",              NULL);   /* no :: */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77:88",           ENTIRE);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77:88:99",        NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "::11:22:33:44:55:66:77",            ENTIRE); /* :: at the start */
  assert_match_anchored (DEFS "(?&IPV6)", "::11:22:33:44:55:66:77:88",         NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33::44:55:66:77",             ENTIRE); /* :: in the middle */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33::44:55:66:77:88",          NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77::",            ENTIRE); /* :: at the end */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77:88::",         NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "::",                                ENTIRE); /* :: only */

  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:192.168.1.1",        NULL);   /* no :: */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:192.168.1.1",     ENTIRE);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66:77:192.168.1.1",  NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "::11:22:33:44:55:192.168.1.1",      ENTIRE); /* :: at the start */
  assert_match_anchored (DEFS "(?&IPV6)", "::11:22:33:44:55:66:192.168.1.1",   NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33::44:55:192.168.1.1",       ENTIRE); /* :: in the imddle */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33::44:55:66:192.168.1.1",    NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55::192.168.1.1",       ENTIRE); /* :: at the end(ish) */
  assert_match_anchored (DEFS "(?&IPV6)", "11:22:33:44:55:66::192.168.1.1",    NULL);
  assert_match_anchored (DEFS "(?&IPV6)", "::192.168.1.1",                     ENTIRE); /* :: only(ish) */

  /* URL_HOST is either a hostname, or an IPv4 address, or a bracket-enclosed IPv6 address */
  assert_match_anchored (DEFS URL_HOST, "example",       ENTIRE);
  assert_match_anchored (DEFS URL_HOST, "example.com",   ENTIRE);
  assert_match_anchored (DEFS URL_HOST, "11.22.33.44",   ENTIRE);
  assert_match_anchored (DEFS URL_HOST, "[11.22.33.44]", NULL);
  assert_match_anchored (DEFS URL_HOST, "dead::be:ef",   "dead");  /* TODO: can/should we totally abort here? */
  assert_match_anchored (DEFS URL_HOST, "[dead::be:ef]", ENTIRE);

  /* EMAIL_HOST is either an at least two-component hostname, or a bracket-enclosed IPv[46] address */
  assert_match_anchored (DEFS EMAIL_HOST, "example",        NULL);
  assert_match_anchored (DEFS EMAIL_HOST, "example.com",    ENTIRE);
  assert_match_anchored (DEFS EMAIL_HOST, "11.22.33.44",    NULL);
  assert_match_anchored (DEFS EMAIL_HOST, "[11.22.33.44]",  ENTIRE);
  assert_match_anchored (DEFS EMAIL_HOST, "[11.22.33.456]", NULL);
  assert_match_anchored (DEFS EMAIL_HOST, "dead::be:ef",    NULL);
  assert_match_anchored (DEFS EMAIL_HOST, "[dead::be:ef]",  ENTIRE);

  /* Number between 1 and 65535 (helper for port) */
  assert_match_anchored (N_1_65535, "0",      NULL);
  assert_match_anchored (N_1_65535, "1",      ENTIRE);
  assert_match_anchored (N_1_65535, "10",     ENTIRE);
  assert_match_anchored (N_1_65535, "100",    ENTIRE);
  assert_match_anchored (N_1_65535, "1000",   ENTIRE);
  assert_match_anchored (N_1_65535, "10000",  ENTIRE);
  assert_match_anchored (N_1_65535, "60000",  ENTIRE);
  assert_match_anchored (N_1_65535, "65000",  ENTIRE);
  assert_match_anchored (N_1_65535, "65500",  ENTIRE);
  assert_match_anchored (N_1_65535, "65530",  ENTIRE);
  assert_match_anchored (N_1_65535, "65535",  ENTIRE);
  assert_match_anchored (N_1_65535, "65536",  NULL);
  assert_match_anchored (N_1_65535, "65540",  NULL);
  assert_match_anchored (N_1_65535, "65600",  NULL);
  assert_match_anchored (N_1_65535, "66000",  NULL);
  assert_match_anchored (N_1_65535, "70000",  NULL);
  assert_match_anchored (N_1_65535, "100000", NULL);
  assert_match_anchored (N_1_65535, "",       NULL);
  assert_match_anchored (N_1_65535, "a1b",    NULL);

  /* PORT is an optional colon-prefixed value */
  assert_match_anchored (PORT, "",       ENTIRE);
  assert_match_anchored (PORT, ":1",     ENTIRE);
  assert_match_anchored (PORT, ":65535", ENTIRE);
  assert_match_anchored (PORT, ":65536", "");     /* TODO: can/should we totally abort here? */

  /* Parentheses are only allowed in matching pairs, see bug 763980. */
  /* TODO: add tests for PATHCHARS and PATHNONTERM; and/or URLPATH */
  assert_match_anchored (DEFS URLPATH, "/ab/cd",       ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/ab/cd.html.", "/ab/cd.html");
  assert_match_anchored (DEFS URLPATH, "/The_Offspring_(album)", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/The_Offspring)", "/The_Offspring");
  assert_match_anchored (DEFS URLPATH, "/a((b(c)d)e(f))", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/a((b(c)d)e(f)))", "/a((b(c)d)e(f))");
  assert_match_anchored (DEFS URLPATH, "/a(b).(c).", "/a(b).(c)");
  assert_match_anchored (DEFS URLPATH, "/a.(b.(c.).).(d.(e.).).)", "/a.(b.(c.).).(d.(e.).)");
  assert_match_anchored (DEFS URLPATH, "/a)b(c", "/a");
  assert_match_anchored (DEFS URLPATH, "/.", "/");
  assert_match_anchored (DEFS URLPATH, "/(.", "/");
  assert_match_anchored (DEFS URLPATH, "/).", "/");
  assert_match_anchored (DEFS URLPATH, "/().", "/()");
  assert_match_anchored (DEFS URLPATH, "/", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/php?param[]=value1&param[]=value2", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/foo?param1[index1]=value1&param2[index2]=value2", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/[[[]][]]", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/[([])]([()])", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/([()])[([])]", ENTIRE);
  assert_match_anchored (DEFS URLPATH, "/[(])", "/");
  assert_match_anchored (DEFS URLPATH, "/([)]", "/");


  /* Put the components together and test the big picture */

  assert_match (REGEX_URL_AS_IS, "There's no URL here http:/foo",               NULL);
  assert_match (REGEX_URL_AS_IS, "Visit http://example.com for details",        "http://example.com");
  assert_match (REGEX_URL_AS_IS, "Trailing dot http://foo/bar.html.",           "http://foo/bar.html");
  assert_match (REGEX_URL_AS_IS, "Trailing ellipsis http://foo/bar.html...",    "http://foo/bar.html");
  assert_match (REGEX_URL_AS_IS, "Trailing comma http://foo/bar,baz,",          "http://foo/bar,baz");
  assert_match (REGEX_URL_AS_IS, "Trailing semicolon http://foo/bar;baz;",      "http://foo/bar;baz");
  assert_match (REGEX_URL_AS_IS, "See <http://foo/bar>",                        "http://foo/bar");
  assert_match (REGEX_URL_AS_IS, "<http://foo.bar/asdf.qwer.html>",             "http://foo.bar/asdf.qwer.html");
  assert_match (REGEX_URL_AS_IS, "Go to http://192.168.1.1.",                   "http://192.168.1.1");
  assert_match (REGEX_URL_AS_IS, "If not, see <http://www.gnu.org/licenses/>.", "http://www.gnu.org/licenses/");
  assert_match (REGEX_URL_AS_IS, "<a href=\"http://foo/bar\">foo</a>",          "http://foo/bar");
  assert_match (REGEX_URL_AS_IS, "<a href='http://foo/bar'>foo</a>",            "http://foo/bar");
  assert_match (REGEX_URL_AS_IS, "<url>http://foo/bar</url>",                   "http://foo/bar");

  assert_match (REGEX_URL_AS_IS, "http://",          NULL);
  assert_match (REGEX_URL_AS_IS, "http://a",         ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://aa.",       "http://aa");
  assert_match (REGEX_URL_AS_IS, "http://aa.b",      ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://aa.bb",     ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://aa.bb/c",   ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://aa.bb/cc",  ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://aa.bb/cc/", ENTIRE);

  assert_match (REGEX_URL_AS_IS, "HtTp://déjà-vu.com:10000/déjà/vu", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "HTTP://joe:sEcReT@➡.ws:1080",      ENTIRE);
  assert_match (REGEX_URL_AS_IS, "https://cömbining-áccents",        ENTIRE);

  assert_match (REGEX_URL_AS_IS, "http://111.222.33.44",                ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://111.222.33.44/",               ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://111.222.33.44/foo",            ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://1.2.3.4:5555/xyz",             ENTIRE);
  assert_match (REGEX_URL_AS_IS, "https://[dead::beef]:12345/ipv6",     ENTIRE);
  assert_match (REGEX_URL_AS_IS, "https://[dead::beef:11.22.33.44]",    ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://1.2.3.4:",                     "http://1.2.3.4");  /* TODO: can/should we totally abort here? */
  assert_match (REGEX_URL_AS_IS, "https://dead::beef/no-brackets-ipv6", "https://dead");    /* ditto */
  assert_match (REGEX_URL_AS_IS, "http://111.222.333.444/",             NULL);
  assert_match (REGEX_URL_AS_IS, "http://1.2.3.4:70000",                "http://1.2.3.4");  /* TODO: can/should we totally abort here? */
  assert_match (REGEX_URL_AS_IS, "http://[dead::beef:111.222.333.444]", NULL);

  /* Username, password */
  assert_match (REGEX_URL_AS_IS, "http://joe@example.com",                 ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://user.name:sec.ret@host.name",     ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://joe:secret@[::1]",                ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://dudewithnopassword:@example.com", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://safeguy:!#$%^&*@host",            ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http://invalidusername!@host",           "http://invalidusername");

  assert_match (REGEX_URL_AS_IS, "http://ab.cd/ef?g=h&i=j|k=l#m=n:o=p", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "http:///foo",                         NULL);

  /* Parentheses are only allowed in matching pairs, see bug 763980. */
  assert_match (REGEX_URL_AS_IS, "https://en.wikipedia.org/wiki/The_Offspring_(album)", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "[markdown](https://en.wikipedia.org/wiki/The_Offspring)", "https://en.wikipedia.org/wiki/The_Offspring");
  assert_match (REGEX_URL_AS_IS, "[markdown](https://en.wikipedia.org/wiki/The_Offspring_(album))", "https://en.wikipedia.org/wiki/The_Offspring_(album)");
  assert_match (REGEX_URL_AS_IS, "[markdown](http://foo.bar/(a(b)c)d)e)f", "http://foo.bar/(a(b)c)d");
  assert_match (REGEX_URL_AS_IS, "[markdown](http://foo.bar/a)b(c", "http://foo.bar/a");

  /* Apostrophes are allowed, except at trailing position if the URL is preceded by an apostrophe, see bug 448044. */
  assert_match (REGEX_URL_AS_IS, "https://en.wikipedia.org/wiki/Moore's_law", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "<a href=\"https://en.wikipedia.org/wiki/Moore's_law\">", "https://en.wikipedia.org/wiki/Moore's_law");
  assert_match (REGEX_URL_AS_IS, "https://en.wikipedia.org/wiki/Cryin'", ENTIRE);
  assert_match (REGEX_URL_AS_IS, "<a href=\"https://en.wikipedia.org/wiki/Cryin'\">", "https://en.wikipedia.org/wiki/Cryin'");
  assert_match (REGEX_URL_AS_IS, "<a href='https://en.wikipedia.org/wiki/Aerosmith'>", "https://en.wikipedia.org/wiki/Aerosmith");

  /* No scheme */
  /* The following tests changed results in PCRE2 revision 1133
   * [https://vcs.pcre.org/pcre2?view=revision&revision=1133] from
   * passing to failing.
   * FIXMEchpe!
   */
  if (!pcre2_atleast_10_34 || pcre2test) {
  assert_match (REGEX_URL_HTTP, "www.foo.bar/baz",     ENTIRE);
  assert_match (REGEX_URL_HTTP, "WWW3.foo.bar/baz",    ENTIRE);
  assert_match (REGEX_URL_HTTP, "FTP.FOO.BAR/BAZ",     ENTIRE);  /* FIXME if no scheme is given and url starts with ftp, can we make the protocol ftp instead of http? */
  assert_match (REGEX_URL_HTTP, "ftpxy.foo.bar/baz",   ENTIRE);
  }
  /* ... and this one from failing to passing */
  if (pcre2_atleast_10_34 || pcre2test) {
  assert_match (REGEX_URL_HTTP, "ftp.123/baz",         NULL);  /* errr... could we fail here?? */
  }
  assert_match (REGEX_URL_HTTP, "foo.bar/baz",         NULL);
  assert_match (REGEX_URL_HTTP, "abc.www.foo.bar/baz", NULL);
  assert_match (REGEX_URL_HTTP, "uvwww.foo.bar/baz",   NULL);
  assert_match (REGEX_URL_HTTP, "xftp.foo.bar/baz",    NULL);

  /* file:/ or file://(hostname)?/ */
  assert_match (REGEX_URL_FILE, "file:",                NULL);
  assert_match (REGEX_URL_FILE, "file:/",               ENTIRE);
  assert_match (REGEX_URL_FILE, "file://",              NULL);
  assert_match (REGEX_URL_FILE, "file:///",             ENTIRE);
  assert_match (REGEX_URL_FILE, "file:////",            NULL);
  assert_match (REGEX_URL_FILE, "file:etc/passwd",      NULL);
  assert_match (REGEX_URL_FILE, "File:/etc/passwd",     ENTIRE);
  assert_match (REGEX_URL_FILE, "FILE:///etc/passwd",   ENTIRE);
  assert_match (REGEX_URL_FILE, "file:////etc/passwd",  NULL);
  assert_match (REGEX_URL_FILE, "file://host.name",     NULL);
  assert_match (REGEX_URL_FILE, "file://host.name/",    ENTIRE);
  assert_match (REGEX_URL_FILE, "file://host.name/etc", ENTIRE);

  assert_match (REGEX_URL_FILE, "See file:/.",             "file:/");
  assert_match (REGEX_URL_FILE, "See file:///.",           "file:///");
  assert_match (REGEX_URL_FILE, "See file:/lost+found.",   "file:/lost+found");
  assert_match (REGEX_URL_FILE, "See file:///lost+found.", "file:///lost+found");

  /* Email */
  assert_match (REGEX_EMAIL, "Write to foo@bar.com.",        "foo@bar.com");
  assert_match (REGEX_EMAIL, "Write to <foo@bar.com>",       "foo@bar.com");
  assert_match (REGEX_EMAIL, "Write to mailto:foo@bar.com.", "mailto:foo@bar.com");
  assert_match (REGEX_EMAIL, "Write to MAILTO:FOO@BAR.COM.", "MAILTO:FOO@BAR.COM");
  assert_match (REGEX_EMAIL, "Write to foo@[1.2.3.4]",       "foo@[1.2.3.4]");
  assert_match (REGEX_EMAIL, "Write to foo@[1.2.3.456]",     NULL);
  assert_match (REGEX_EMAIL, "Write to foo@[1::2345]",       "foo@[1::2345]");
  assert_match (REGEX_EMAIL, "Write to foo@[dead::beef]",    "foo@[dead::beef]");
  assert_match (REGEX_EMAIL, "Write to foo@1.2.3.4",         NULL);
  assert_match (REGEX_EMAIL, "Write to foo@1.2.3.456",       NULL);
  assert_match (REGEX_EMAIL, "Write to foo@1::2345",         NULL);
  assert_match (REGEX_EMAIL, "Write to foo@dead::beef",      NULL);
  assert_match (REGEX_EMAIL, "<baz email=\"foo@bar.com\"/>", "foo@bar.com");
  assert_match (REGEX_EMAIL, "<baz email='foo@bar.com'/>",   "foo@bar.com");
  assert_match (REGEX_EMAIL, "<email>foo@bar.com</email>",   "foo@bar.com");

  /* Sip, examples from rfc 3261 */
  assert_match (REGEX_URL_VOIP, "sip:alice@atlanta.com;maddr=239.255.255.1;ttl=15",           ENTIRE);
  assert_match (REGEX_URL_VOIP, "sip:alice@atlanta.com",                                      ENTIRE);
  assert_match (REGEX_URL_VOIP, "sip:alice:secretword@atlanta.com;transport=tcp",             ENTIRE);
  assert_match (REGEX_URL_VOIP, "sips:alice@atlanta.com?subject=project%20x&priority=urgent", ENTIRE);
  assert_match (REGEX_URL_VOIP, "sip:+1-212-555-1212:1234@gateway.com;user=phone",            ENTIRE);
  assert_match (REGEX_URL_VOIP, "sips:1212@gateway.com",                                      ENTIRE);
  assert_match (REGEX_URL_VOIP, "sip:alice@192.0.2.4",                                        ENTIRE);
  assert_match (REGEX_URL_VOIP, "sip:atlanta.com;method=REGISTER?to=alice%40atlanta.com",     ENTIRE);
  assert_match (REGEX_URL_VOIP, "SIP:alice;day=tuesday@atlanta.com",                          ENTIRE);
  assert_match (REGEX_URL_VOIP, "Dial sip:alice@192.0.2.4.",                                  "sip:alice@192.0.2.4");

  /* Extremely long match, bug 770147 */
  assert_match (REGEX_URL_AS_IS, "http://www.example.com/ThisPathConsistsOfMoreThan1024Characters"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890"
                                 "1234567890123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890", ENTIRE);
}

static void
test_regex_unicode(void)
{
        GError* error{nullptr};
        g_assert_true(vte::base::Regex::check_pcre_config_unicode(&error));
        g_assert_no_error(error);
}

static bool
parse_args(char*** argv,
           int* argc,
           GError** error)
{
        auto _pcre2test = gboolean{FALSE};
        GOptionEntry const entries[] = {
                { "pcre2test", 0, 0, G_OPTION_ARG_NONE, &_pcre2test,
                  "Print tests in pcre2test format to stdout", nullptr },
                { nullptr }
        };

        auto context = g_option_context_new(nullptr);
        g_option_context_set_help_enabled(context, false);
        g_option_context_set_ignore_unknown_options(context, true);
        g_option_context_add_main_entries(context, entries, nullptr);

        bool rv = g_option_context_parse(context, argc, argv, error);
        g_option_context_free(context);

        pcre2test = _pcre2test != false;
        return rv;
}

int
main(int argc,
     char* argv[])
{
        setlocale(LC_ALL, "");

        g_test_init(&argc, &argv, nullptr);

        GError* err{nullptr};
        if (!parse_args(&argv, &argc, &err)) {
                g_printerr("Failed to parse arguments: %s\n", err->message);
                g_error_free(err);
                return EXIT_FAILURE;
        }

        auto version = vte::base::Regex::get_pcre_version();
        pcre2_atleast_10_34 = strverscmp(version.c_str(), "10.34") > 0;

        if (pcre2test)
                g_print("#pattern multiline,ucp,utf,no_utf_check\n\n");

        /* Build test suites */

        g_test_add_func("/vte/regex/unicode", test_regex_unicode);

        setup_regex_builtins_tests();

        /* Run tests */

        if (pcre2test)
                return EXIT_SUCCESS;

        return g_test_run();
}
