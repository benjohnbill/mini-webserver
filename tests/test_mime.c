/* Unit tests for extension -> MIME type mapping. */
#include "server.h"
#include "test.h"

int main(void)
{
    CHECK_STR(mime_type_for_path("/index.html"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("a.htm"), "text/html; charset=utf-8");
    CHECK_STR(mime_type_for_path("/css/style.css"), "text/css; charset=utf-8");
    CHECK_STR(mime_type_for_path("/js/app.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("mod.mjs"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path("d.json"), "application/json; charset=utf-8");
    CHECK_STR(mime_type_for_path("r.txt"), "text/plain; charset=utf-8");
    CHECK_STR(mime_type_for_path("logo.png"), "image/png");
    CHECK_STR(mime_type_for_path("p.jpg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("p.jpeg"), "image/jpeg");
    CHECK_STR(mime_type_for_path("a.gif"), "image/gif");
    CHECK_STR(mime_type_for_path("i.svg"), "image/svg+xml; charset=utf-8");
    CHECK_STR(mime_type_for_path("favicon.ico"), "image/x-icon");
    CHECK_STR(mime_type_for_path("a.webp"), "image/webp");
    CHECK_STR(mime_type_for_path("doc.pdf"), "application/pdf");
    CHECK_STR(mime_type_for_path("m.wasm"), "application/wasm");
    CHECK_STR(mime_type_for_path("feed.xml"), "application/xml; charset=utf-8");

    /* case-insensitive */
    CHECK_STR(mime_type_for_path("/IMG/PHOTO.JPG"), "image/jpeg");
    CHECK_STR(mime_type_for_path("Index.HtMl"), "text/html; charset=utf-8");

    /* unknown / no extension / tricky names */
    CHECK_STR(mime_type_for_path("archive.tar.gz"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("Makefile"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/dir.html/file"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/.htaccess"), "application/octet-stream");
    CHECK_STR(mime_type_for_path("trailing."), "application/octet-stream");
    CHECK_STR(mime_type_for_path("/a/b.min.js"), "text/javascript; charset=utf-8");
    CHECK_STR(mime_type_for_path(NULL), "application/octet-stream");

    TEST_MAIN_END();
}
