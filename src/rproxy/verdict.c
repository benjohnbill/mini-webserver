#include "verdict.h"

#include <stddef.h>
#include <string.h>

/* Source: Cloudflare "AI Scrapers and Crawlers" rule, ai.robots.txt, JA4,
   fail2ban (jail.conf maxretry and findtime, the nginx-botsearch filter),
   Cloudflare verified bots and pay per crawl, Anubis (Proof of work). */

/* The rule table: the first rule that matches decides. */
struct verdict_rule {
    bool (*matches)(const struct verdict_signals *signals);
    enum verdict_kind kind;
    bool blocks_client;
};

/* fail2ban maxretry: 5 failures in the window (findtime) ban the client.
   The window is the window of the behavior counters. */
#define SCANNER_NOT_FOUND_MAX 5
/* The same maxretry for 429 replies: a client that is over the limit
   again after five 429s in the window goes into the block table, so its
   next connections drop before the TLS handshake. Only with the IP key:
   a JA4 limit is shared by all clients of one TLS library, so its 429s
   are no evidence against one IP. */
#define LIMIT_SLOWED_MAX 5
#define BROWSER_LABEL "browser"

/* User-Agent tokens of AI and SEO crawlers that name themselves. */
static const char *const crawler_names[] = {
    "GPTBot", "CCBot", "ClaudeBot", "Bytespider", "PerplexityBot", "AhrefsBot",
};

static bool request_is_in_honeypot(const struct verdict_signals *signals);
static bool client_is_scanner(const struct verdict_signals *signals);
static bool bot_is_verified(const struct verdict_signals *signals);
static bool crawler_must_pay(const struct verdict_signals *signals);
static bool agent_is_crawler(const struct verdict_signals *signals);
static bool agent_lies_about_chrome(const struct verdict_signals *signals);
static bool request_needs_pow(const struct verdict_signals *signals);
static bool client_keeps_over_limit(const struct verdict_signals *signals);
static bool request_is_over_limit(const struct verdict_signals *signals);

/* The two rules that block come first: they are the strongest evidence,
   and a crawler that names itself but ignores robots.txt is caught too.
   They hold for a verified bot too. A verified bot then passes, so it
   skips the User-Agent, JA4, Proof of work, and rate limit rules. Pay
   comes before the crawler deny, so a pay target gets 402, not 403. */
static const struct verdict_rule rules[] = {
    {request_is_in_honeypot, VERDICT_TARPIT, true},
    {client_is_scanner, VERDICT_DENY, true},
    {bot_is_verified, VERDICT_PASS, false},
    {crawler_must_pay, VERDICT_PAY, false},
    {agent_is_crawler, VERDICT_DENY, false},
    {agent_lies_about_chrome, VERDICT_TARPIT, false},
    {request_needs_pow, VERDICT_CHALLENGE, false},
    {client_keeps_over_limit, VERDICT_SLOW, true},
    {request_is_over_limit, VERDICT_SLOW, false},
};

struct verdict verdict_decide(const struct verdict_signals *signals)
{
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (rules[i].matches(signals)) {
            int retry_after =
                rules[i].kind == VERDICT_SLOW ? signals->retry_after : 0;
            return (struct verdict){.kind = rules[i].kind,
                                    .retry_after = retry_after,
                                    .blocks_client = rules[i].blocks_client};
        }
    }

    return (struct verdict){.kind = VERDICT_PASS};
}

/* robots.txt disallows the Honeypot, and a person does not see its link. */
static bool request_is_in_honeypot(const struct verdict_signals *signals)
{
    return strncmp(signals->target, BEHAVIOR_TRAP_PREFIX,
                   strlen(BEHAVIOR_TRAP_PREFIX)) == 0;
}

/* Many 404 replies in the window: a client that guesses paths. */
static bool client_is_scanner(const struct verdict_signals *signals)
{
    return signals->behavior.not_found >= SCANNER_NOT_FOUND_MAX;
}

static bool bot_is_verified(const struct verdict_signals *signals)
{
    return signals->bot_verified;
}

/* A crawler that names itself, with no valid signature (that rule came
   first), on a --pay-prefix target. */
static bool crawler_must_pay(const struct verdict_signals *signals)
{
    return signals->is_pay_target && agent_is_crawler(signals);
}

static bool agent_is_crawler(const struct verdict_signals *signals)
{
    if (signals->user_agent == NULL) {
        return false;
    }
    for (size_t i = 0; i < sizeof(crawler_names) / sizeof(crawler_names[0]);
         i++) {
        if (strstr(signals->user_agent, crawler_names[i]) != NULL) {
            return true;
        }
    }

    return false;
}

/* The User-Agent says Chrome, but the JA4 table knows the JA4 and does
   not call it a browser. An unknown JA4 is no evidence: a real browser
   that is not in the table must pass. */
static bool agent_lies_about_chrome(const struct verdict_signals *signals)
{
    return signals->user_agent != NULL &&
           strstr(signals->user_agent, "Chrome/") != NULL &&
           signals->ja4_label != NULL &&
           strcmp(signals->ja4_label, BROWSER_LABEL) != 0;
}

static bool request_needs_pow(const struct verdict_signals *signals)
{
    return signals->is_pow_target && !signals->pow_ok;
}

static bool client_keeps_over_limit(const struct verdict_signals *signals)
{
    return request_is_over_limit(signals) && signals->limit_is_per_ip &&
           signals->behavior.slowed >= LIMIT_SLOWED_MAX;
}

static bool request_is_over_limit(const struct verdict_signals *signals)
{
    return signals->retry_after > 0;
}
