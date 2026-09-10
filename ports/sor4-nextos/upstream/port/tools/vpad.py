#!/usr/bin/env python3
# vpad.py - cria um gamepad VIRTUAL via uinput clonando o " USB Gamepad " 0810:0001
# (mesmo bus/vendor/product/version/nome/caps -> SDL calcula a MESMA GUID e aplica o
# mesmo mapping interno -> o jogo le como se fosse o pad real). Depois injeta presses.
# uso: vpad.py hold <ms> <btncode...>   (segura os botoes por ms)
#      vpad.py tap  <ms> <btncode...>   (idem, com release)
#      vpad.py seq  <ms> <btncode...>   (pressiona cada btn 1 a 1 por ms, com gap)
#      vpad.py keep                     (so cria e mantem vivo 8s, sem press)
import os, struct, fcntl, time, sys, ctypes

UINPUT="/dev/uinput"
EV_SYN,EV_KEY,EV_ABS=0x00,0x01,0x03
SYN_REPORT=0
# ioctl helpers
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
UI_SET_EVBIT =_IOC(1,ord('U'),100,4)
UI_SET_KEYBIT=_IOC(1,ord('U'),101,4)
UI_SET_ABSBIT=_IOC(1,ord('U'),103,4)
UI_DEV_CREATE=_IOC(0,ord('U'),1,0)
UI_DEV_DESTROY=_IOC(0,ord('U'),2,0)

BTNS=[288,289,290,291,292,293,294,295,296,297,298,299]
ABSES=[0,1,2,5,16,17]  # X,Y,Z,RZ,HAT0X,HAT0Y
ABS_MIN={0:0,1:0,2:0,5:0,16:-1,17:-1}
ABS_MAX={0:255,1:255,2:255,5:255,16:1,17:1}
ABS_FLAT={0:15,1:15,2:15,5:15,16:0,17:0}

def create():
    fd=os.open(UINPUT,os.O_WRONLY|os.O_NONBLOCK)
    fcntl.ioctl(fd,UI_SET_EVBIT,EV_KEY)
    fcntl.ioctl(fd,UI_SET_EVBIT,EV_ABS)
    fcntl.ioctl(fd,UI_SET_EVBIT,EV_SYN)
    for b in BTNS: fcntl.ioctl(fd,UI_SET_KEYBIT,b)
    for a in ABSES: fcntl.ioctl(fd,UI_SET_ABSBIT,a)
    # struct uinput_user_dev: char name[80]; input_id{u16 bustype,vendor,product,version};
    #   u32 ff_effects_max; s32 absmax[64]; s32 absmin[64]; s32 absfuzz[64]; s32 absflat[64];
    name=b" USB Gamepad          "
    name=name+b"\x00"*(80-len(name))
    bus,ven,prod,ver=0x0003,0x0810,0x0001,0x0110
    idblob=struct.pack("HHHH",bus,ven,prod,ver)
    ff=struct.pack("I",0)
    absmax=[0]*64; absmin=[0]*64; absfuzz=[0]*64; absflat=[0]*64
    for a in ABSES:
        absmax[a]=ABS_MAX[a]; absmin[a]=ABS_MIN[a]; absflat[a]=ABS_FLAT[a]
    def arr(x): return struct.pack("64i",*x)
    blob=name+idblob+ff+arr(absmax)+arr(absmin)+arr(absfuzz)+arr(absflat)
    os.write(fd,blob)
    fcntl.ioctl(fd,UI_DEV_CREATE)
    return fd

def emit(fd,etype,code,val):
    # input_event: struct timeval{long sec,long usec}; u16 type; u16 code; s32 value
    ev=struct.pack("llHHi",0,0,etype,code,val)
    os.write(fd,ev)

def syn(fd): emit(fd,EV_SYN,SYN_REPORT,0)

def main():
    cmd=sys.argv[1] if len(sys.argv)>1 else "keep"
    fd=create()
    # estado de repouso dos eixos (centrados) p/ SDL nao ver lixo
    for a in (0,1,2,5):
        emit(fd,EV_ABS,a,128 if a in (2,5) else 127)
    for a in (16,17): emit(fd,EV_ABS,a,0)
    syn(fd)
    time.sleep(2.0)   # deixa SDL/udev enumerar e abrir o controller
    if cmd=="keep":
        time.sleep(8);
    elif cmd in ("hold","tap"):
        ms=int(sys.argv[2]); btns=[int(x) for x in sys.argv[3:]]
        for b in btns: emit(fd,EV_KEY,b,1)
        syn(fd); time.sleep(ms/1000.0)
        if cmd=="tap":
            for b in btns: emit(fd,EV_KEY,b,0)
            syn(fd); time.sleep(0.3)
    elif cmd=="seq":
        ms=int(sys.argv[2]); btns=[int(x) for x in sys.argv[3:]]
        for b in btns:
            sys.stderr.write("[VPAD] press btn %d\n"%b); sys.stderr.flush()
            emit(fd,EV_KEY,b,1); syn(fd); time.sleep(ms/1000.0)
            emit(fd,EV_KEY,b,0); syn(fd); time.sleep(0.4)
    time.sleep(0.5)
    try: fcntl.ioctl(fd,UI_DEV_DESTROY)
    except Exception: pass
    os.close(fd)

if __name__=="__main__": main()
