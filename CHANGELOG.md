Zurl Changelog
==============

v. 1.5.1 (2016-02-21)

  * Don't install tests.
  * Fix crash when using --version.
  * Output compact JSON (to match 1.4 behavior).

v. 1.5.0 (2016-02-12)

  * Port to Qt 5.
  * Drop libqjson dependency since Qt 5 has JSON support built-in.

v. 1.4.10 (2015-09-24)

  * Fix buffer corruption on large uploads.

v. 1.4.9 (2015-07-16)

  * WebSocket: DNS queries now respect search domains in resolv.conf.
  * WebSocket: fix for error responses with no Content-Length.

v. 1.4.8 (2015-07-07)

  * Be explicit about types passed to curl_easy_setopt (fixes 32-bit systems).

v. 1.4.7 (2015-07-05)

  * DNS: always do absolute query, even if not first.

v. 1.4.6 (2015-07-05)

  * DNS queries now respect search domains in resolv.conf.

v. 1.4.5 (2015-06-26)

  * Fix regression with GET requests.
  * Ensure responses with no content (HEAD, 204, 304) are handled properly.

v. 1.4.4 (2015-06-26)

  * Fix HEAD requests.

v. 1.4.3 (2015-06-25)

  * Allow request body with GET and DELETE.

v. 1.4.2 (2015-06-18)

  * Support redirects when there is a request body.
  * Fix bool types when using JSON as the message format.

v. 1.4.1 (2015-06-12)

  * Fix type conversions when using JSON as the message format.

v. 1.4.0 (2015-06-11)

  * follow-redirects option.
  * timeout option.
  * id is now optional.
  * Ack stream requests immediately so they become referenceable.
  * Respond with headers even before initial body received.

v. 1.3.1 (2014-09-16)

v. 1.3.0 (2014-02-13)

v. 1.1.0 (2013-11-24)

v. 1.0.0 (2013-08-10)

  * Stable version.
