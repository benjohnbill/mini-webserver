#include "verdict.h"

#include <stddef.h>
#include <string.h>

/* Source: Cloudflare "AI Scrapers and Crawlers" rule, ai.robots.txt, JA4. */

/* The rule table: the first rule that matches decides. */
struct verdict_rule {
    bool (*matches)(const struct verdict_signals *signals);
    enum verdict_kind kind;
};

/* User-Agent tokens of AI and SEO crawlers that name themselves. */
static const char *const crawler_names[] = {
    "GPTBot", "CCBot", "ClaudeBot", "Bytespider", "PerplexityBot", "AhrefsBot",
};

/* The JA4 cipher hash (ja4_b) of Chrome and the other Chromium browsers. */
#define CHROMIUM_CIPHER_HASH "8daaf6152771"
#define CIPHER_HASH_AT 11

static bool agent_is_crawler(const struct verdict_signals *signals);
static bool agent_lies_about_chrome(const struct verdict_signals *signals);
static bool request_is_over_limit(const struct verdict_signals *signals);

static const struct verdict_rule rules[] = {
    {agent_is_crawler, VERDICT_DENY},
    {agent_lies_about_chrome, VERDICT_TARPIT},
    {request_is_over_limit, VERDICT_SLOW},
};

struct verdict verdict_decide(const struct verdict_signals *signals)
{
    for (size_t i = 0; i < sizeof(rules) / sizeof(rules[0]); i++) {
        if (rules[i].matches(signals)) {
            int retry_after =
                rules[i].kind == VERDICT_SLOW ? signals->retry_after : 0;
            return (struct verdict){rules[i].kind, retry_after};
        }
    }

    return (struct verdict){VERDICT_PASS, 0};
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

/* The User-Agent says Chrome, but the TLS is not Chromium. With no JA4
   there is no evidence. */
static bool agent_lies_about_chrome(const struct verdict_signals *signals)
{
    if (signals->user_agent == NULL ||
        strstr(signals->user_agent, "Chrome/") == NULL ||
        strlen(signals->ja4) < CIPHER_HASH_AT + strlen(CHROMIUM_CIPHER_HASH)) {
        return false;
    }

    return strncmp(signals->ja4 + CIPHER_HASH_AT, CHROMIUM_CIPHER_HASH,
                   strlen(CHROMIUM_CIPHER_HASH)) != 0;
}

static bool request_is_over_limit(const struct verdict_signals *signals)
{
    return signals->retry_after > 0;
}
