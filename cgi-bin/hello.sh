#!/bin/sh
# Minimal CGI script: print a header block, an empty line, then the body.
# QUERY_STRING is client-controlled, so HTML-escape it before echoing.
escape() {
    printf '%s' "$1" | sed -e 's/&/\&amp;/g' -e 's/</\&lt;/g' \
                           -e 's/>/\&gt;/g' -e 's/"/\&quot;/g'
}

printf 'Content-Type: text/html; charset=utf-8\r\n'
printf '\r\n'
cat <<EOF
<!DOCTYPE html>
<html><head><title>hello.sh</title></head>
<body>
<h1>Hello from CGI</h1>
<ul>
  <li>REQUEST_METHOD: $(escape "${REQUEST_METHOD:-}")</li>
  <li>QUERY_STRING: $(escape "${QUERY_STRING:-}")</li>
  <li>REMOTE_ADDR: $(escape "${REMOTE_ADDR:-}")</li>
</ul>
<p><a href="/">back</a></p>
</body></html>
EOF
