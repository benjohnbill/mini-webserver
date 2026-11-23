#include <stdio.h>

#include "check.h"
#include "verdict.h"

/* The first two tests are the rules that SPEC 11 and GLOSSARY.md fix. The
   others are the rest of the Verdict table (SPEC 11 row 3). The crawler
   User-Agents are the ones that each crawler documents. The Chromium JA4
   is the Chrome example of the FoxIO JA4 README. The other JA4 is the
   openssl s_client JA4 of tests/test_ja4.c. */

static struct verdict decide(const char *user_agent, int retry_after)
{
    struct verdict_signals signals = {"", user_agent, "/", retry_after};

    return verdict_decide(&signals);
}

static struct verdict decide_with_ja4(const char *user_agent, const char *ja4)
{
    struct verdict_signals signals = {ja4, user_agent, "/", 0};

    return verdict_decide(&signals);
}

#define CHROME_AGENT                                                           \
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "      \
    "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36"

static void verification_clients_within_the_limit_pass(void)
{
    static const char *agents[] = {"curl/8.18.0", "ApacheBench/2.3", "wrk",
                                   "hey/0.0.1", NULL};
    for (size_t i = 0; i < sizeof(agents) / sizeof(agents[0]); i++) {
        CHECK_EQ_INT(decide(agents[i], 0).kind, VERDICT_PASS);
    }
}

static void a_request_over_the_rate_limit_is_slowed_with_its_retry_after(void)
{
    struct verdict verdict = decide("curl/8.18.0", 7);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK_EQ_INT(verdict.retry_after, 7);
}

static void a_known_ai_or_seo_crawler_is_denied(void)
{
    static const char *agents[] = {
        "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; "
        "GPTBot/1.1; +https://openai.com/gptbot)",
        "CCBot/2.0 (https://commoncrawl.org/faq/)",
        "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; "
        "ClaudeBot/1.0; +claudebot@anthropic.com)",
        "Mozilla/5.0 (Linux; Android 5.0) AppleWebKit/537.36 (KHTML, like "
        "Gecko) Mobile Safari/537.36 (compatible; Bytespider; "
        "spider-feedback@bytedance.com)",
        "Mozilla/5.0 AppleWebKit/537.36 (KHTML, like Gecko; compatible; "
        "PerplexityBot/1.0; +https://perplexity.ai/perplexitybot)",
        "Mozilla/5.0 (compatible; AhrefsBot/7.0; +http://ahrefs.com/robot/)",
    };
    for (size_t i = 0; i < sizeof(agents) / sizeof(agents[0]); i++) {
        CHECK_EQ_INT(decide(agents[i], 0).kind, VERDICT_DENY);
    }
}

static void a_crawler_over_the_rate_limit_is_denied_not_slowed(void)
{
    struct verdict verdict = decide("GPTBot/1.1", 7);
    CHECK_EQ_INT(verdict.kind, VERDICT_DENY);
    CHECK_EQ_INT(verdict.retry_after, 0);
}

static void a_chrome_user_agent_with_a_chromium_ja4_passes(void)
{
    CHECK_EQ_INT(
        decide_with_ja4(CHROME_AGENT, "t13d1516h2_8daaf6152771_02713d6af862")
            .kind,
        VERDICT_PASS);
}

static void a_chrome_user_agent_with_a_non_chromium_ja4_is_tarpitted(void)
{
    CHECK_EQ_INT(
        decide_with_ja4(CHROME_AGENT, "t13d3013h2_1d37bd780c83_ecd0401ec68b")
            .kind,
        VERDICT_TARPIT);
}

static void a_chrome_user_agent_without_a_ja4_passes(void)
{
    CHECK_EQ_INT(decide_with_ja4(CHROME_AGENT, "").kind, VERDICT_PASS);
}

int main(void)
{
    verification_clients_within_the_limit_pass();
    a_request_over_the_rate_limit_is_slowed_with_its_retry_after();
    a_known_ai_or_seo_crawler_is_denied();
    a_crawler_over_the_rate_limit_is_denied_not_slowed();
    a_chrome_user_agent_with_a_chromium_ja4_passes();
    a_chrome_user_agent_with_a_non_chromium_ja4_is_tarpitted();
    a_chrome_user_agent_without_a_ja4_passes();
    printf("test_verdict: ok\n");

    return 0;
}
