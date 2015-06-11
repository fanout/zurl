Zurl
====
Author: Justin Karneges <justin@fanout.io>  
Mailing List: http://lists.fanout.io/listinfo.cgi/fanout-users-fanout.io

Description
-----------

Zurl is an HTTP and WebSocket client daemon with a ZeroMQ interface. To make an asynchronous HTTP request, simply send a message to Zurl using your favorite language.

```python
import json
import zmq

# set up zmq socket
sock = zmq.Context.instance().socket(zmq.REQ)
sock.connect('ipc:///tmp/zurl-req')

# send request
req = {
  'method': 'GET',
  'uri': 'https://fanout.io/'
}
sock.send('J' + json.dumps(req))

# print response
print json.loads(sock.recv()[1:])
```

Every language can already make HTTP requests directly, so what good is this? Zurl is mainly useful for implementing Webhooks, because applications don't need to keep state or worry about concurrency. Zurl even offers protection from evil URLs.

Zurl can also make sense as part of a greater ZeroMQ-based architecture, where you want to integrate HTTP itself into your pipeline.

See [Fun With Zurl](http://blog.fanout.io/2014/02/18/fun-with-zurl-the-http-websocket-client-daemon/) for some wild possibilities that a message-oriented HTTP client daemon can bring. We hope you will be either inspired or horrified.

License
-------

Zurl is offered under the GNU GPL. See the COPYING file.

Features
--------

  * Request HTTP and HTTPS URLs
  * Connect to WS and WSS URLs for WebSockets
  * HTTP support based on Libcurl
  * Event-driven design can handle thousands of simultaneous connections
  * Two access methods: REQ and PUSH/SUB
  * Requests and responses can be buffered in single messages or streamed
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

    python tools/get.py https://fanout.io/
