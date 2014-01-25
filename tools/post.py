import sys
import time
import uuid
import tnetstring
import zmq

client_id = 'post.py'

ctx = zmq.Context()
out_sock = ctx.socket(zmq.PUSH)
out_sock.connect('ipc:///tmp/zurl-in')
out_stream_sock = ctx.socket(zmq.ROUTER)
out_stream_sock.connect('ipc:///tmp/zurl-in-stream')
in_sock = ctx.socket(zmq.SUB)
in_sock.setsockopt(zmq.SUBSCRIBE, client_id)
in_sock.connect('ipc:///tmp/zurl-out')

time.sleep(0.5)

pos = 0
file = open(sys.argv[2], 'r').read()

rid = str(uuid.uuid4())
outseq = 0
outcredits = 0
out_sock.send('T' + tnetstring.dumps({'from': client_id, 'id': rid, 'seq': outseq, 'method': 'POST', 'uri': sys.argv[1], 'headers': [['Content-Length', str(len(file))]], 'stream': True, 'credits': 200000, 'more': True}))
outseq += 1

raddr = None

while True:
	buf = in_sock.recv()
	at = buf.find(' ')
	receiver = buf[:at]
	data = tnetstring.loads(buf[at + 2:])
	print 'IN: %s %s' % (receiver, data)
	if ('type' in data and data['type'] == 'error') or ('type' not in data and 'more' not in data):
		break
	if 'type' in data and data['type'] == 'keep-alive':
		odata = {'id': rid, 'from': client_id, 'seq': outseq, 'type': 'keep-alive'}
		print 'OUT: %s' % odata
		out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(odata)])
		outseq += 1
		continue
	if 'credits' in data:
		outcredits += data['credits']
	raddr = data['from']
	if 'body' in data and len(data['body']) > 0:
		odata = {'id': rid, 'from': client_id, 'seq': outseq, 'type': 'credit', 'credits': len(data['body'])}
		print 'OUT: %s' % odata
		out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(odata)])
		outseq += 1
	if pos < len(file) and outcredits > 0:
		chunk = file[pos:pos + outcredits]
		outcredits -= len(chunk)
		pos += len(chunk)
		odata = {'id': rid, 'from': client_id, 'seq': outseq, 'body': chunk}
		print len(chunk), pos, len(file)
		if pos < len(file):
			odata['more'] = True
		print 'OUT: %s' % odata
		out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(odata)])
		outseq += 1
