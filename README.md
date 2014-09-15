Zurl
====
Author: Justin Karneges <justin@fanout.io>  
Mailing List: http://lists.fanout.io/listinfo.cgi/fanout-users-fanout.io

Description
-----------

HTTP and WebSocket client worker with ZeroMQ interface.

License
-------

Zurl is offered under the GNU GPL. See the COPYING file.

Features
--------

  * Request HTTP and HTTPS URLs
  * Connect to WS and WSS URLs for WebSockets
  * Completely event-driven, using JDNS and Libcurl
  * Handle thousands of simultaneous outbound connections
  * Two access methods: REQ and PUSH/SUB (think Mongrel2 in reverse!)
  * Streaming requests and responses
  * Packet format can be JSON or TNetStrings
  * Set access policies (e.g. block requests to 10.*)

Requirements
------------

  * qt >= 4.7
  * libzmq >= 2.0
  * libcurl >= 7.20
  * qjson

Setup
-----

If accessing from Git, be sure to pull submodules:

    git submodule init
    git submodule update

Build:

    ./configure
    make

Run:

    cp zurl.conf.example zurl.conf
    ./zurl --verbose --config=zurl.conf

Test:

    python tools/get.py http://fanout.io/
