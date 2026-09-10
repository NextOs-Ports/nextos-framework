#!/usr/bin/env python3
# autopad.py - cria 1 gamepad virtual (clone " USB Gamepad " 0810:0001) e MARTELA SO o A
# sem parar (sem DOWN - DOWN entra em Options e estraga a navegacao). Normal eh o default,
# entao A-spam: titulo -> Story -> Normal -> fase. Roda ate ser morto.
# Mapeamento ativo (Twin USB PS2 Adapter, GUID 0300...100800000100000010010000): a:b2 = evdev 290.
# Uso: python3 autopad.py [code] [period_ms]   (default code=290=A, period=300ms)
import os, struct, fcntl, time, sys
UINPUT="/dev/uinput"
EV_SYN,EV_KEY,EV_ABS=0,1,3
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
UI_SET_EVBIT=_IOC(1,ord('U'),100,4); UI_SET_KEYBIT=_IOC(1,ord('U'),101,4)
UI_SET_ABSBIT=_IOC(1,ord('U'),103,4); UI_DEV_CREATE=_IOC(0,ord('U'),1,0); UI_DEV_DESTROY=_IOC(0,ord('U'),2,0)
BTNS=list(range(288,300)); ABSES=[0,1,2,5,16,17]
AMIN={0:0,1:0,2:0,5:0,16:-1,17:-1}; AMAX={0:255,1:255,2:255,5:255,16:1,17:1}; AFLAT={0:15,1:15,2:15,5:15,16:0,17:0}
def create():
    fd=os.open(UINPUT,os.O_WRONLY|os.O_NONBLOCK)
    for e in (EV_KEY,EV_ABS,EV_SYN): fcntl.ioctl(fd,UI_SET_EVBIT,e)
    for b in BTNS: fcntl.ioctl(fd,UI_SET_KEYBIT,b)
    for a in ABSES: fcntl.ioctl(fd,UI_SET_ABSBIT,a)
    name=b" USB Gamepad          "; name=name+b"\x00"*(80-len(name))
    blob=name+struct.pack("HHHH",0x0003,0x0810,0x0001,0x0110)+struct.pack("I",0)
    mx=[0]*64; mn=[0]*64; fz=[0]*64; fl=[0]*64
    for a in ABSES: mx[a]=AMAX[a]; mn[a]=AMIN[a]; fl[a]=AFLAT[a]
    blob+=struct.pack("64i",*mx)+struct.pack("64i",*mn)+struct.pack("64i",*fz)+struct.pack("64i",*fl)
    os.write(fd,blob); fcntl.ioctl(fd,UI_DEV_CREATE); return fd
def emit(fd,t,c,v): os.write(fd,struct.pack("llHHi",0,0,t,c,v))
def syn(fd): emit(fd,EV_SYN,0,0)
def main():
    code=int(sys.argv[1]) if len(sys.argv)>1 else 290
    per=(int(sys.argv[2]) if len(sys.argv)>2 else 300)/1000.0
    fd=create()
    for a in (0,1,2,5): emit(fd,EV_ABS,a,128 if a in(2,5) else 127)
    emit(fd,EV_ABS,16,0); emit(fd,EV_ABS,17,0); syn(fd)
    sys.stderr.write("[AUTOPAD] martelando code=%d cada %dms\n"%(code,int(per*1000))); sys.stderr.flush()
    n=0
    try:
        while True:
            emit(fd,EV_KEY,code,1); syn(fd); time.sleep(0.10)
            emit(fd,EV_KEY,code,0); syn(fd); time.sleep(per)
            n+=1
            if n%20==0: sys.stderr.write("[AUTOPAD] %d taps de A\n"%n); sys.stderr.flush()
    finally:
        try: fcntl.ioctl(fd,UI_DEV_DESTROY)
        except Exception: pass
        os.close(fd)
if __name__=="__main__":
    try: main()
    except (KeyboardInterrupt,SystemExit): pass
