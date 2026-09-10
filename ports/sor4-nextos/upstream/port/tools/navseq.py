#!/usr/bin/env python3
# navseq.py - avanca o menu do SoR4 injetando START+A ALTERNADO direto no /dev/input/eventN
# do pad REAL (id=0). NAO cria pad novo (pad novo viraria id=1 e o menu nao escuta + quebra
# o pad real). Mapeamento Twin USB PS2 (a:b2=290, start:b9=297). Roda 'cycles' e sai sozinho.
# uso: navseq.py <eventdev> <cycles> [start_code] [a_code] [period_ms]
import os, struct, time, sys
dev=sys.argv[1]
cycles=int(sys.argv[2]) if len(sys.argv)>2 else 40
START=int(sys.argv[3]) if len(sys.argv)>3 else 297
A=int(sys.argv[4]) if len(sys.argv)>4 else 290
per=(int(sys.argv[5]) if len(sys.argv)>5 else 500)/1000.0
fd=os.open(dev, os.O_WRONLY)
def ev(t,c,v): os.write(fd, struct.pack("llHHi",0,0,t,c,v))
def tap(code):
    ev(1,code,1); ev(0,0,0); time.sleep(0.10)   # down+SYN
    ev(1,code,0); ev(0,0,0)                       # up+SYN
for i in range(cycles):
    tap(START); time.sleep(per)
    tap(A);     time.sleep(per)
    sys.stderr.write("[NAV] cycle %d start+a\n"%(i+1)); sys.stderr.flush()
os.close(fd)
sys.stderr.write("[NAV] fim (%d ciclos)\n"%cycles); sys.stderr.flush()
