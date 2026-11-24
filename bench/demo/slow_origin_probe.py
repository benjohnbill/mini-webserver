#!/usr/bin/env python3
# Usage: bench/demo/slow_origin_probe.py PORT N
# Opens N slow readers of /update.bin straight to an Origin for 6 s and
# prints the KB that each got. A pool Origin moves only as many as it has
# Workers (SPEC 12, "Facts").
import socket, sys, time, threading
port=int(sys.argv[1]); n=int(sys.argv[2]); got=[0]*n
def phone(i):
    s=socket.create_connection(("127.0.0.1",port))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 65536)
    s.sendall(b"GET /update.bin HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
    end=time.time()+6
    while time.time()<end:
        try:
            s.settimeout(0.2); b=s.recv(4096)
        except socket.timeout: continue
        if not b: break
        got[i]+=len(b); time.sleep(0.01)   # ~400 KB/s
    s.close()
ts=[threading.Thread(target=phone,args=(i,)) for i in range(n)]
[t.start() for t in ts]; [t.join() for t in ts]
print("bytes per phone after 6s:", [g//1000 for g in got], "KB")
print("phones that got any byte:", sum(1 for g in got if g), "of", n)
