#include <stdbool.h>
#include <stdio.h>

#include "check.h"
#include "verdict.h"

/* The rules of SPEC 11 row 3 and of the v2 layers. The crawler
   User-Agents are the ones that each crawler documents. The JA4 labels
   are the labels of the JA4 table (--ja4-list). */

static struct verdict decide(const char *user_agent, int retry_after)
{
    struct verdict_signals signals = {
        .user_agent = user_agent, .target = "/", .retry_after = retry_after};

    return verdict_decide(&signals);
}

static struct verdict decide_with_label(const char *user_agent,
                                        const char *ja4_label)
{
    struct verdict_signals signals = {
        .ja4_label = ja4_label, .user_agent = user_agent, .target = "/"};

    return verdict_decide(&signals);
}

static struct verdict decide_for_target(const char *user_agent,
                                        const char *target)
{
    struct verdict_signals signals = {.user_agent = user_agent,
                                      .target = target};

    return verdict_decide(&signals);
}

static struct verdict decide_with_not_found(unsigned not_found)
{
    struct verdict_signals signals = {
        .user_agent = "curl/8.18.0",
        .target = "/",
        .behavior = {.requests = not_found + 1, .not_found = not_found}};

    return verdict_decide(&signals);
}

static struct verdict decide_with_slowed(unsigned slowed, int retry_after,
                                         bool limit_is_per_ip)
{
    struct verdict_signals signals = {
        .user_agent = "curl/8.18.0",
        .target = "/",
        .retry_after = retry_after,
        .limit_is_per_ip = limit_is_per_ip,
        .behavior = {.requests = slowed + 1, .slowed = slowed}};

    return verdict_decide(&signals);
}

static struct verdict decide_for_pow(const char *target, bool is_pow_target,
                                     bool pow_ok, int retry_after)
{
    struct verdict_signals signals = {.user_agent = "curl/8.18.0",
                                      .target = target,
                                      .retry_after = retry_after,
                                      .is_pow_target = is_pow_target,
                                      .pow_ok = pow_ok};

    return verdict_decide(&signals);
}

static struct verdict decide_for_pay(const char *user_agent, bool is_pay_target,
                                     bool bot_verified)
{
    struct verdict_signals signals = {.user_agent = user_agent,
                                      .target = "/data/1.html",
                                      .is_pay_target = is_pay_target,
                                      .bot_verified = bot_verified};

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

static void a_chrome_user_agent_with_a_browser_ja4_passes(void)
{
    CHECK_EQ_INT(decide_with_label(CHROME_AGENT, "browser").kind, VERDICT_PASS);
}

static void a_chrome_user_agent_with_a_known_non_browser_ja4_is_tarpitted(void)
{
    CHECK_EQ_INT(decide_with_label(CHROME_AGENT, "curl").kind, VERDICT_TARPIT);
    CHECK_EQ_INT(decide_with_label(CHROME_AGENT, "python").kind,
                 VERDICT_TARPIT);
}

static void a_chrome_user_agent_with_an_unknown_ja4_passes(void)
{
    CHECK_EQ_INT(decide_with_label(CHROME_AGENT, NULL).kind, VERDICT_PASS);
}

static void a_curl_user_agent_with_the_curl_ja4_passes(void)
{
    CHECK_EQ_INT(decide_with_label("curl/8.18.0", "curl").kind, VERDICT_PASS);
}

static void a_false_chrome_is_not_put_in_the_block_table(void)
{
    CHECK(!decide_with_label(CHROME_AGENT, "curl").blocks_client);
}

static void a_request_for_the_honeypot_is_tarpitted_and_blocks_the_client(void)
{
    static const char *targets[] = {"/trap/", "/trap/page.html", "/trap/?x=1"};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        struct verdict verdict = decide_for_target("curl/8.18.0", targets[i]);
        CHECK_EQ_INT(verdict.kind, VERDICT_TARPIT);
        CHECK(verdict.blocks_client);
    }
}

static void a_target_outside_the_honeypot_passes(void)
{
    static const char *targets[] = {"/trap", "/traps/", "/robots.txt",
                                    "/index.html?/trap/"};
    for (size_t i = 0; i < sizeof(targets) / sizeof(targets[0]); i++) {
        struct verdict verdict = decide_for_target("curl/8.18.0", targets[i]);
        CHECK_EQ_INT(verdict.kind, VERDICT_PASS);
        CHECK(!verdict.blocks_client);
    }
}

static void a_named_crawler_in_the_honeypot_is_tarpitted_not_denied(void)
{
    struct verdict verdict = decide_for_target("GPTBot/1.1", "/trap/");
    CHECK_EQ_INT(verdict.kind, VERDICT_TARPIT);
    CHECK(verdict.blocks_client);
}

static void a_client_with_five_404s_in_the_window_is_denied_and_blocked(void)
{
    struct verdict verdict = decide_with_not_found(5);
    CHECK_EQ_INT(verdict.kind, VERDICT_DENY);
    CHECK(verdict.blocks_client);
}

static void a_client_with_four_404s_in_the_window_passes(void)
{
    struct verdict verdict = decide_with_not_found(4);
    CHECK_EQ_INT(verdict.kind, VERDICT_PASS);
    CHECK(!verdict.blocks_client);
}

static void a_denied_crawler_and_a_slowed_client_are_not_blocked(void)
{
    CHECK(!decide("GPTBot/1.1", 0).blocks_client);
    CHECK(!decide("curl/8.18.0", 7).blocks_client);
}

static void a_client_over_the_limit_after_five_429s_is_slowed_and_blocked(void)
{
    struct verdict verdict = decide_with_slowed(5, 7, true);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK_EQ_INT(verdict.retry_after, 7);
    CHECK(verdict.blocks_client);
}

static void a_client_over_the_limit_after_four_429s_is_not_blocked(void)
{
    struct verdict verdict = decide_with_slowed(4, 7, true);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK(!verdict.blocks_client);
}

static void a_client_within_the_limit_after_five_429s_passes(void)
{
    struct verdict verdict = decide_with_slowed(5, 0, true);
    CHECK_EQ_INT(verdict.kind, VERDICT_PASS);
    CHECK(!verdict.blocks_client);
}

static void a_client_over_a_ja4_limit_after_five_429s_is_not_blocked(void)
{
    struct verdict verdict = decide_with_slowed(5, 7, false);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK(!verdict.blocks_client);
}

static void a_pow_target_without_a_solution_gets_the_challenge(void)
{
    struct verdict verdict = decide_for_pow("/data/1.html", true, false, 0);
    CHECK_EQ_INT(verdict.kind, VERDICT_CHALLENGE);
    CHECK(!verdict.blocks_client);
}

static void a_pow_target_with_a_solution_passes(void)
{
    CHECK_EQ_INT(decide_for_pow("/data/1.html", true, true, 0).kind,
                 VERDICT_PASS);
}

static void a_target_outside_the_pow_prefix_passes_with_no_solution(void)
{
    CHECK_EQ_INT(decide_for_pow("/index.html", false, false, 0).kind,
                 VERDICT_PASS);
}

static void a_client_over_the_limit_on_a_pow_target_gets_the_challenge(void)
{
    struct verdict verdict = decide_for_pow("/data/1.html", true, false, 7);
    CHECK_EQ_INT(verdict.kind, VERDICT_CHALLENGE);
    CHECK_EQ_INT(verdict.retry_after, 0);
}

static void a_solved_pow_target_over_the_limit_is_slowed(void)
{
    struct verdict verdict = decide_for_pow("/data/1.html", true, true, 7);
    CHECK_EQ_INT(verdict.kind, VERDICT_SLOW);
    CHECK_EQ_INT(verdict.retry_after, 7);
}

static void a_false_chrome_on_a_pow_target_is_tarpitted_not_challenged(void)
{
    struct verdict_signals signals = {.ja4_label = "curl",
                                      .user_agent = CHROME_AGENT,
                                      .target = "/data/1.html",
                                      .is_pow_target = true};
    CHECK_EQ_INT(verdict_decide(&signals).kind, VERDICT_TARPIT);
}

static void the_honeypot_under_a_pow_prefix_is_tarpitted_not_challenged(void)
{
    struct verdict verdict = decide_for_pow("/trap/", true, false, 0);
    CHECK_EQ_INT(verdict.kind, VERDICT_TARPIT);
    CHECK(verdict.blocks_client);
}

static void a_named_crawler_on_a_pay_target_must_pay(void)
{
    struct verdict verdict = decide_for_pay("GPTBot/1.1", true, false);
    CHECK_EQ_INT(verdict.kind, VERDICT_PAY);
    CHECK(!verdict.blocks_client);
}

static void a_named_crawler_outside_the_pay_prefix_is_denied(void)
{
    CHECK_EQ_INT(decide_for_pay("GPTBot/1.1", false, false).kind, VERDICT_DENY);
}

static void a_client_that_is_not_a_crawler_on_a_pay_target_passes(void)
{
    CHECK_EQ_INT(decide_for_pay("curl/8.18.0", true, false).kind, VERDICT_PASS);
    CHECK_EQ_INT(decide_for_pay(NULL, true, false).kind, VERDICT_PASS);
}

static void a_verified_bot_with_a_crawler_user_agent_passes(void)
{
    CHECK_EQ_INT(decide_for_pay("GPTBot/1.1", true, true).kind, VERDICT_PASS);
    CHECK_EQ_INT(decide_for_pay("GPTBot/1.1", false, true).kind, VERDICT_PASS);
}

static void a_verified_bot_skips_the_false_chrome_rule(void)
{
    struct verdict_signals signals = {.ja4_label = "curl",
                                      .user_agent = CHROME_AGENT,
                                      .target = "/",
                                      .bot_verified = true};
    CHECK_EQ_INT(verdict_decide(&signals).kind, VERDICT_PASS);
}

static void a_verified_bot_skips_the_challenge_and_the_rate_limit(void)
{
    struct verdict_signals signals = {.user_agent = "GPTBot/1.1",
                                      .target = "/data/1.html",
                                      .retry_after = 7,
                                      .is_pow_target = true,
                                      .bot_verified = true};
    struct verdict verdict = verdict_decide(&signals);
    CHECK_EQ_INT(verdict.kind, VERDICT_PASS);
    CHECK_EQ_INT(verdict.retry_after, 0);
}

static void a_verified_bot_in_the_honeypot_is_tarpitted_and_blocked(void)
{
    struct verdict_signals signals = {
        .user_agent = "GPTBot/1.1", .target = "/trap/", .bot_verified = true};
    struct verdict verdict = verdict_decide(&signals);
    CHECK_EQ_INT(verdict.kind, VERDICT_TARPIT);
    CHECK(verdict.blocks_client);
}

static void a_verified_bot_with_five_404s_is_denied_and_blocked(void)
{
    struct verdict_signals signals = {
        .user_agent = "GPTBot/1.1",
        .target = "/",
        .bot_verified = true,
        .behavior = {.requests = 6, .not_found = 5}};
    struct verdict verdict = verdict_decide(&signals);
    CHECK_EQ_INT(verdict.kind, VERDICT_DENY);
    CHECK(verdict.blocks_client);
}

int main(void)
{
    verification_clients_within_the_limit_pass();
    a_request_over_the_rate_limit_is_slowed_with_its_retry_after();
    a_known_ai_or_seo_crawler_is_denied();
    a_crawler_over_the_rate_limit_is_denied_not_slowed();
    a_chrome_user_agent_with_a_browser_ja4_passes();
    a_chrome_user_agent_with_a_known_non_browser_ja4_is_tarpitted();
    a_chrome_user_agent_with_an_unknown_ja4_passes();
    a_curl_user_agent_with_the_curl_ja4_passes();
    a_false_chrome_is_not_put_in_the_block_table();
    a_request_for_the_honeypot_is_tarpitted_and_blocks_the_client();
    a_target_outside_the_honeypot_passes();
    a_named_crawler_in_the_honeypot_is_tarpitted_not_denied();
    a_client_with_five_404s_in_the_window_is_denied_and_blocked();
    a_client_with_four_404s_in_the_window_passes();
    a_denied_crawler_and_a_slowed_client_are_not_blocked();
    a_client_over_the_limit_after_five_429s_is_slowed_and_blocked();
    a_client_over_the_limit_after_four_429s_is_not_blocked();
    a_client_within_the_limit_after_five_429s_passes();
    a_client_over_a_ja4_limit_after_five_429s_is_not_blocked();
    a_pow_target_without_a_solution_gets_the_challenge();
    a_pow_target_with_a_solution_passes();
    a_target_outside_the_pow_prefix_passes_with_no_solution();
    a_client_over_the_limit_on_a_pow_target_gets_the_challenge();
    a_solved_pow_target_over_the_limit_is_slowed();
    a_false_chrome_on_a_pow_target_is_tarpitted_not_challenged();
    the_honeypot_under_a_pow_prefix_is_tarpitted_not_challenged();
    a_named_crawler_on_a_pay_target_must_pay();
    a_named_crawler_outside_the_pay_prefix_is_denied();
    a_client_that_is_not_a_crawler_on_a_pay_target_passes();
    a_verified_bot_with_a_crawler_user_agent_passes();
    a_verified_bot_skips_the_false_chrome_rule();
    a_verified_bot_skips_the_challenge_and_the_rate_limit();
    a_verified_bot_in_the_honeypot_is_tarpitted_and_blocked();
    a_verified_bot_with_five_404s_is_denied_and_blocked();
    printf("test_verdict: ok\n");

    return 0;
}
