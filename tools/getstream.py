import sys
import time
import uuid
import tnetstring
import zmq

client_id = 'getstream.py'

ctx = zmq.Context()
out_sock = ctx.socket(zmq.PUSH)
out_sock.connect('ipc:///tmp/zurl-in')
out_stream_sock = ctx.socket(zmq.ROUTER)
out_stream_sock.connect('ipc:///tmp/zurl-in-stream')
in_sock = ctx.socket(zmq.SUB)
in_sock.setsockopt(zmq.SUBSCRIBE, client_id)
in_sock.connect('ipc:///tmp/zurl-out')

time.sleep(0.5)

rid = str(uuid.uuid4())
inseq = 0
outseq = 0
out_sock.send('T' + tnetstring.dumps({'from': client_id, 'id': rid, 'seq': outseq, 'method': 'GET', 'uri': sys.argv[1], 'stream': True, 'credits': 230000}))
outseq += 1

while True:
	buf = in_sock.recv()
	at = buf.find(' ')
	receiver = buf[:at]
	indata = tnetstring.loads(buf[at + 2:])
	if indata['id'] != rid:
		continue
	print 'IN: %s' % indata
	assert(indata['seq'] == inseq)
	inseq += 1
	if ('type' in indata and (indata['type'] == 'error' or indata['type'] == 'cancel')) or ('type' not in indata and 'more' not in indata):
		break
	raddr = indata['from']
	if 'body' in indata and len(indata['body']) > 0:
		outdata = {'id': rid, 'from': client_id, 'seq': outseq, 'type': 'credit', 'credits': len(indata['body'])}
		print 'OUT: %s' % outdata
		out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(outdata)])
		outseq += 1
