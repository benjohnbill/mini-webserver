#!/usr/bin/env python3
"""CGI form handler: reads an application/x-www-form-urlencoded POST body
from stdin (exactly CONTENT_LENGTH bytes) and lists the fields as HTML."""
import html
import os
import sys
from urllib.parse import parse_qsl


def main():
    method = os.environ.get("REQUEST_METHOD", "GET")
    try:
        length = int(os.environ.get("CONTENT_LENGTH") or 0)
    except ValueError:
        length = 0

    if method == "POST" and length > 0:
        raw = sys.stdin.buffer.read(length).decode("utf-8", "replace")
    else:
        raw = os.environ.get("QUERY_STRING", "")

    fields = parse_qsl(raw, keep_blank_values=True)
    rows = "\n".join(
        "  <tr><td>{}</td><td>{}</td></tr>".format(html.escape(k), html.escape(v))
        for k, v in fields
    ) or '  <tr><td colspan="2">(no fields)</td></tr>'

    body = """<!DOCTYPE html>
<html><head><title>form.py</title></head>
<body>
<h1>Form fields ({method})</h1>
<table border="1">
{rows}
</table>
<p><a href="/">back</a></p>
</body></html>
""".format(method=html.escape(method), rows=rows)

    out = sys.stdout.buffer
    out.write(b"Content-Type: text/html; charset=utf-8\r\n\r\n")
    out.write(body.encode("utf-8"))
    out.flush()


if __name__ == "__main__":
    main()
