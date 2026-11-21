#include <stdio.h>
#include <string.h>

#include "check.h"
#include "mime.h"

static void html_is_text_html(void)
{
    CHECK(strcmp(mime_find_type("www/index.html"), "text/html") == 0);
}

static void png_is_image_png(void)
{
    CHECK(strcmp(mime_find_type("www/image.png"), "image/png") == 0);
}

static void an_unknown_extension_is_octet_stream(void)
{
    CHECK(strcmp(mime_find_type("www/large.bin"), "application/octet-stream") ==
          0);
}

static void a_dot_in_a_folder_name_is_not_an_extension(void)
{
    CHECK(strcmp(mime_find_type("www/v1.css/file"),
                 "application/octet-stream") == 0);
}

int main(void)
{
    html_is_text_html();
    png_is_image_png();
    an_unknown_extension_is_octet_stream();
    a_dot_in_a_folder_name_is_not_an_extension();
    printf("test_mime: ok\n");
    return 0;
}
