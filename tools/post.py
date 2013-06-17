import sys
import time
import uuid
import tnetstring
import zmq

client_id = "post.py"

ctx = zmq.Context()
out_sock = ctx.socket(zmq.PUSH)
out_sock.connect("tcp://127.0.0.1:5550")
out_stream_sock = ctx.socket(zmq.ROUTER)
out_stream_sock.connect("tcp://127.0.0.1:5551")
in_sock = ctx.socket(zmq.SUB)
in_sock.setsockopt(zmq.SUBSCRIBE, client_id)
in_sock.connect("tcp://127.0.0.1:5552")

pos = 0
file = open(sys.argv[2], "r").read()

rid = str(uuid.uuid4())
outseq = 0
outcredits = 0
out_sock.send(tnetstring.dumps({"from": client_id, "id": rid, "seq": outseq, "method": "POST", "uri": sys.argv[1], "headers": [["Content-Length", str(len(file))]], "stream": True, "credits": 200000, "more": True}))
outseq += 1

while True:
	buf = in_sock.recv()
	at = buf.find(" ")
	receiver = buf[:at]
	data = tnetstring.loads(buf[at + 1:])
	print receiver, data
	if ("type" in data and data["type"] == "error") or ("type" not in data and "more" not in data):
		break
	if "credits" in data:
		outcredits += data["credits"]
	raddr = data["reply-address"]
	if "body" in data and len(data["body"]) > 0:
		out_stream_sock.send_multipart([raddr, "", tnetstring.dumps({"id": rid, "sender": client_id, "seq": outseq, "credits": len(data["body"])})])
		outseq += 1
	if pos < len(file) and outcredits > 0:
		chunk = file[pos:pos + outcredits]
		outcredits -= len(chunk)
		pos += len(chunk)
		odata = {"id": rid, "sender": client_id, "seq": outseq, "body": chunk}
		print len(chunk), pos, len(file)
		if pos < len(file):
			odata["more"] = True
		out_stream_sock.send_multipart([raddr, "", tnetstring.dumps(odata)])
		outseq += 1
		print "wrote chunk"
