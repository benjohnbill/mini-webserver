#include <stdio.h>
#include <string.h>

#include "check.h"
#include "response.h"

static bool header_has(const struct response_plan *plan, const char *text);

static void a_200_has_its_status_line_and_content_length(void)
{
    struct response_plan plan;
    CHECK(response_plan_init(&plan, 200, "text/html", 5, false));
    CHECK(strncmp(plan.header, "HTTP/1.1 200 OK\r\n", 17) == 0);
    CHECK(header_has(&plan, "\r\nContent-Length: 5\r\n"));
    CHECK(header_has(&plan, "\r\nConnection: keep-alive\r\n"));
    CHECK(!plan.close_after);
    CHECK_EQ_INT(plan.file_fd, -1);
    response_plan_close(&plan);
}

static void a_404_has_its_status_line_and_content_length(void)
{
    struct response_plan plan;
    response_plan_init_error(&plan, 404, true);
    CHECK(strncmp(plan.header, "HTTP/1.1 404 Not Found\r\n", 24) == 0);
    CHECK(header_has(&plan, "\r\nContent-Length: 14\r\n"));
    CHECK(header_has(&plan, "\r\nConnection: close\r\n"));
    CHECK(header_has(&plan, "\r\n\r\n404 Not Found\n"));
    CHECK(plan.close_after);
    response_plan_close(&plan);
}

static void a_405_has_an_allow_header(void)
{
    struct response_plan plan;
    response_plan_init_not_allowed(&plan, "POST");
    CHECK(strncmp(plan.header, "HTTP/1.1 405 Method Not Allowed\r\n", 33) == 0);
    CHECK(header_has(&plan, "\r\nAllow: POST\r\n"));
    response_plan_close(&plan);
}

static void a_content_type_too_long_for_the_header_becomes_a_500(void)
{
    char content_type[2000];
    memset(content_type, 't', sizeof(content_type) - 1);
    content_type[sizeof(content_type) - 1] = '\0';
    struct response_plan plan;
    CHECK(!response_plan_init(&plan, 200, content_type, 5, false));
    CHECK(strncmp(plan.header, "HTTP/1.1 500 Internal Server Error\r\n", 36) ==
          0);
    CHECK(plan.close_after);
    response_plan_close(&plan);
}

int main(void)
{
    a_200_has_its_status_line_and_content_length();
    a_404_has_its_status_line_and_content_length();
    a_405_has_an_allow_header();
    a_content_type_too_long_for_the_header_becomes_a_500();
    printf("test_response: ok\n");

    return 0;
}

static bool header_has(const struct response_plan *plan, const char *text)
{
    char header[sizeof(plan->header) + 1];
    memcpy(header, plan->header, plan->header_len);
    header[plan->header_len] = '\0';

    return strstr(header, text) != NULL;
}
