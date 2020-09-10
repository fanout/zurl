# Zurl

Author: Justin Karneges <justin@fanout.io>

## Description

Zurl is an HTTP and WebSocket client daemon with a ZeroMQ interface. Send it a message to make an HTTP request.

For example, here's how to make a request using Python:

```python
import json
import zmq

# set up zmq socket
sock = zmq.Context.instance().socket(zmq.REQ)
sock.connect('ipc:///tmp/zurl-req')

# send request
req = {
  'method': 'GET',
  'uri': 'http://example.com/path'
}
sock.send('J' + json.dumps(req))

# print response
print json.loads(sock.recv()[1:])
```

Since every language can already make HTTP requests directly, you might wonder what value there is in delegating the work to a separate process like this. Zurl is mainly useful for implementing Webhooks, because applications don't need to keep state nor worry about concurrency. Zurl even offers protection from [evil URLs](http://blog.fanout.io/2014/01/27/how-to-safely-invoke-webhooks/).

Zurl can also make sense as part of a greater ZeroMQ-based architecture, where you want to integrate HTTP itself into your pipeline.

See [Fun With Zurl](http://blog.fanout.io/2014/02/18/fun-with-zurl-the-http-websocket-client-daemon/) for some wild possibilities that a message-oriented HTTP client daemon can bring.

## License

Zurl is offered under the GNU GPL. See the COPYING file.

## Features

  * Request HTTP and HTTPS URLs
  * Connect to WS and WSS URLs for WebSockets
  * HTTP support based on Libcurl
  * Event-driven design can handle thousands of simultaneous connections
  * Two access methods: REQ and PUSH/SUB
  * Requests and responses can be buffered in single messages or streamed
  * Packet format can be JSON or TNetStrings
  * Set access policies (e.g. block requests to 10.*)

## Requirements

  * qt >= 5.2
  * libzmq >= 2.0
  * libcurl >= 7.20

## Setup

If accessing from Git, be sure to pull submodules:

    git submodule init
    git submodule update

Build:

    ./configure --qtselect=5
    make

Run:

    cp zurl.conf.example zurl.conf
    ./zurl --verbose --config=zurl.conf

## Message Format

Requests and response messages are encoded in JSON or TNetStrings format. The format type is indicated by prefixing the encoded output with either a 'J' character or a 'T' character, respectively.

For example, a request message in JSON format might look like this:

```
J{"method":"GET","uri":"http://example.com/"}
```

Here's an example of the same request in TNetStrings format:

```
T44:6:method,3:GET,3:uri,19:http://example.com/,}
```

Here's what a response might look like:

```
J{"code":"200","reason":"OK","headers":[...],"body":"hello\n"}
```

Zurl always replies using the same format that was used in the request. If you need to send and receive binary content, you'll need to use TNetString format rather than JSON (Zurl does not attempt to Base64-encode binary content over JSON or anything like that).

Requests may have a number of fields. Here are the main ones:

* ``id`` - Unique ID among requests sent.
* ``method`` - The HTTP method to use.
* ``uri`` - The full URI to make the request to, e.g. scheme://domain.com/path?query
* ``headers`` - The request headers as a list of two-item lists.
* ``body`` - The request body content.

Only ``method`` and ``uri`` are required. Headers are not strictly required, not even ``Content-Length`` as Zurl will set that header for you. If ``body`` is unspecified, it is assumed to be empty. If you are using a REQ socket to speak with Zurl, then you can probably get away with having no ``id`` field. However, if you use DEALER for multiplexing, then you'll need to ID your requests in order to be able to match them to responses.

Additional request fields:

* ``user-data`` - Arbitrary data to be echoed back in the response message. It's a handy way to ship off state with the request, if the response will be handled by a separate process.
* ``max-size`` - Don't accept a response body larger than this value.
* ``connect-host`` - Override the host to connect to. The outgoing ``Host`` header will still be derived from the URI.
* ``connect-port`` - Override the port to connect to.
* ``ignore-policies`` - Ignore any rules about what requests are allowed (i.e. bypass Zurl's allow/deny rules).
* ``ignore-tls-errors`` - Ignore the certificate of the server when using HTTPS or WSS.
* ``follow-redirects`` - If a 3xx response code with a ``Location`` header is received, follow the redirect (up to 8 redirects before failing).
* ``timeout`` - Maximum time in milliseconds for the entire request/response operation.

Responses may have the following fields:

* ``id`` - The ID of the request.
* ``type`` - Either ommitted or with value ``error``, meaning the request failed in some way.
* ``condition`` - In an error response, this is a short, machine-parsable string indicating the reason for the error.
* ``code`` - The HTTP status code.
* ``reason`` - The HTTP status reason (e.g. "OK").
* ``headers`` - The response headers as a list of two-item lists.
* ``body`` - The response body content.
* ``user-data`` - If this field was specified in the request, then it will be included in the response.

## Sockets

For basic usage, connect to Zurl's request-based interface using a REQ socket (ipc:///tmp/zurl-req by default, see your zurl.conf). To make a request, send a message over the socket. To receive the response, read from the socket.

For advanced usage you can connect to Zurl's streaming interface using PUSH, ROUTER, and SUB sockets. See tools/getstream.py as an example or check out the [ZHTTP draft spec](http://rfc.zeromq.org/spec:33) for details.

## WebSockets

Creating a WebSocket connection through Zurl uses a variant of the ZHTTP protocol. Zurl's streaming interface must be used in this case. The protocol is not documented yet, but you can see tools/wsecho.py as an example.
