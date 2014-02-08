import sys
import time
import uuid
import tnetstring
import zmq

client_id = 'wsecho.py'

ctx = zmq.Context()
out_sock = ctx.socket(zmq.PUSH)
out_sock.connect('ipc:///tmp/zurl-in')
out_stream_sock = ctx.socket(zmq.ROUTER)
out_stream_sock.connect('ipc:///tmp/zurl-in-stream')
in_sock = ctx.socket(zmq.SUB)
in_sock.setsockopt(zmq.SUBSCRIBE, client_id)
in_sock.connect('ipc:///tmp/zurl-out')

# ensure subscription
time.sleep(0.5)

rid = str(uuid.uuid4())
inseq = 0
outseq = 0
state = 0 # 0=connecting, 1=connected, 2=closing

m = dict()
m['from'] = client_id
m['id'] = rid
m['seq'] = outseq
m['uri'] = sys.argv[1]
m['credits'] = 200000
print 'OUT: %s' % m
out_sock.send('T' + tnetstring.dumps(m))
outseq += 1

while True:
	m_raw = in_sock.recv()
	at = m_raw.find(' ')
	m = tnetstring.loads(m_raw[at + 2:])
	if m['id'] != rid:
		continue
	print 'IN: %s' % m
	assert(m['seq'] == inseq)
	inseq += 1

	if state == 0:
		mtype = m.get('type')
		if mtype is not None:
			break

		# connected
		state = 1
		raddr = m['from']

		# send a message
		m = dict()
		m['from'] = client_id
		m['id'] = rid
		m['seq'] = outseq
		m['body'] = 'hello world'
		print 'OUT: %s' % m
		out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(m)])
		outseq += 1
	elif state == 1:
		# did we receive data?
		if 'type' not in m:
			state = 2

			# send close
			m = dict()
			m['from'] = client_id
			m['id'] = rid
			m['seq'] = outseq
			m['type'] = 'close'
			print 'OUT: %s' % m
			out_stream_sock.send_multipart([raddr, '', 'T' + tnetstring.dumps(m)])
			outseq += 1
	elif state == 2:
		if m.get('type') == 'close':
			state = 0
			break
