#!/usr/bin/env python3
"""Run shell commands in the Ubuntu release's maintenance RAM image (piano.diag=maintenance: telnetd on its USB NCM
address, port 2323, root shell, no password; xiaomipad-8pro-mainline docs/FLASHING.en.md). Telnet option negotiation is refused.
   maint-shell.py 'COMMAND' [TIMEOUT_SECONDS]      prints the output, exits with the command's status
"""
import os, socket, sys, time, re
# The image's fixed USB-link address (see its docs), kept in ~/.config/piano/env as PIANO_MAINT_HOST=...
def env_host():
    if os.environ.get('PIANO_MAINT_HOST'): return os.environ['PIANO_MAINT_HOST']
    for line in open(os.path.expanduser('~/.config/piano/env')):
        if line.startswith('PIANO_MAINT_HOST='): return line.split('=', 1)[1].strip().strip('"')
    sys.exit('PIANO_MAINT_HOST is not set (environment or ~/.config/piano/env)')
HOST, PORT = env_host(), 2323
cmd = sys.argv[1]; timeout = float(sys.argv[2]) if len(sys.argv) > 2 else 120
s = socket.create_connection((HOST, PORT), timeout=10)
s.settimeout(0.5)
buf = b''
def pump(until=None, deadline=None):
    global buf
    while True:
        try:
            d = s.recv(65536)
            if not d: break
            out = bytearray(); i = 0
            while i < len(d):                       # refuse every telnet option
                if d[i] == 255 and i + 2 < len(d) and d[i+1] in (251, 252, 253, 254):
                    s.sendall(bytes([255, 254 if d[i+1] in (251, 252) else 252, d[i+2]])); i += 3
                else:
                    out.append(d[i]); i += 1
            buf += bytes(out)
        except socket.timeout:
            pass
        if until and until in buf: return True
        if deadline and time.time() > deadline: return False
pump(deadline=time.time() + 3)
mark = f'__PIANO_DONE_{int(time.time())}__'
# the marker is assembled by the shell, so the echoed command line never contains it
s.sendall((f'stty -echo 2>/dev/null; {cmd}; printf "%s%s=%d\\n" __PIANO_DONE_ {mark[len("__PIANO_DONE_"):]} $?\n').encode())
buf = b''
ok = pump(until=mark.encode() + b'=', deadline=time.time() + timeout)
pump(deadline=time.time() + 0.5)
text = buf.decode(errors='replace')
m = re.search(re.escape(mark) + r'=(\d+)', text)
body = text[:text.find(mark)] if mark in text else text
sys.stdout.write(re.sub(r'^.*?stty -echo[^\n]*\n', '', body, count=1, flags=re.S))
sys.exit(int(m.group(1)) if m else 124)
