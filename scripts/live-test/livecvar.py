# [LIVE-CVAR] Local tuning admin module (no stdlib imports: this server's Python
# cannot load pylib). Every ~1 s it re-reads live_cvars.txt; when the content
# changes, each non-empty, non-comment line runs as a console command and the
# result is appended to live_cvars.log. The content present at start is not run.
import host

_state = {'frame': 0, 'last': None}

def _read():
	try:
		f = open('live_cvars.txt', 'r')
		data = f.read()
		f.close()
		return data
	except:
		return None

def init():
	_state['last'] = _read()
	try:
		f = open('live_cvars.log', 'a')
		f.write('livecvar: init\n')
		f.close()
	except:
		pass

def shutdown():
	pass

def update():
	_state['frame'] = _state['frame'] + 1
	if _state['frame'] % 30: return
	data = _read()
	if data is None or data == _state['last']: return
	_state['last'] = data
	try:
		log = open('live_cvars.log', 'a')
		for line in data.split('\n'):
			line = line.strip()
			if not line or line[0] == '#': continue
			res = host.rcon_invoke(line)
			log.write('frame %d: %s -> %s\n' % (_state['frame'], line, str(res).strip()))
		log.close()
	except:
		pass
