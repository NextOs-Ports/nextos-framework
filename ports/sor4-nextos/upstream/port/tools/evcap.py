#!/usr/bin/env python3
# evcap.py - le /dev/input/event2 (pad real) e imprime cada botao/eixo com timestamp.
# Usado p/ descobrir o mapeamento FISICO do pad do porter.
import struct, time, sys, os
dev = sys.argv[1] if len(sys.argv)>1 else "/dev/input/event2"
dur = float(sys.argv[2]) if len(sys.argv)>2 else 40
fd = os.open(dev, os.O_RDONLY)
sz = struct.calcsize("llHHi")
t0 = time.time()
print("CAPTURANDO %ds de %s..."%(dur,dev)); sys.stdout.flush()
import fcntl
fl = fcntl.fcntl(fd, fcntl.F_GETFL); fcntl.fcntl(fd, fcntl.F_SETFL, fl | os.O_NONBLOCK)
import select
while time.time()-t0 < dur:
    r,_,_=select.select([fd],[],[],0.3)
    if fd not in r: continue
    try: data=os.read(fd, sz*64)
    except OSError: continue
    for i in range(0,len(data),sz):
        sec,usec,etype,code,val = struct.unpack("llHHi", data[i:i+sz])
        if etype==1:  # EV_KEY
            print("%.1f BTN code=%d val=%d"%(time.time()-t0, code, val)); sys.stdout.flush()
        elif etype==3:  # EV_ABS
            if code in (16,17) or abs(val-127)>40 or abs(val-128)>40 or val in (0,255):
                print("%.1f ABS code=%d val=%d"%(time.time()-t0, code, val)); sys.stdout.flush()
os.close(fd)
print("FIM")
