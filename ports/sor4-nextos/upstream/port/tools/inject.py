#!/usr/bin/env python3
# inject.py - injeta eventos de botao DIRETO num /dev/input/eventN existente (id=0),
# em vez de criar um pad novo (que viraria id=1 e o menu nao escuta). Sem uinput.
# uso: inject.py <eventdev> <code> <count> [period_ms]
import os, struct, time, sys
dev=sys.argv[1]; code=int(sys.argv[2]); n=int(sys.argv[3]) if len(sys.argv)>3 else 20
per=(int(sys.argv[4]) if len(sys.argv)>4 else 300)/1000.0
fd=os.open(dev, os.O_WRONLY)
def ev(t,c,v): os.write(fd, struct.pack("llHHi",0,0,t,c,v))
for i in range(n):
    ev(1,code,1); ev(0,0,0); time.sleep(0.10)   # EV_KEY down + SYN
    ev(1,code,0); ev(0,0,0); time.sleep(per)     # EV_KEY up + SYN
os.close(fd)
sys.stderr.write("[INJECT] %d taps code=%d em %s\n"%(n,code,dev))
