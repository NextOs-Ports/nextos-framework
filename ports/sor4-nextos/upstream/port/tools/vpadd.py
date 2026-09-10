#!/usr/bin/env python3
# vpadd.py - DAEMON: cria 1 gamepad virtual via uinput (clone do " USB Gamepad " 0810:0001)
# e o mantem VIVO, lendo comandos de uma FIFO. SDL ve UM controller estavel.
# comandos (1 por linha na fifo /tmp/vpadcmd):
#   p <code>         press (down)         r <code>  release (up)
#   t <code> <ms>    tap (down,wait,up)   hx <-1|0|1>  dpad X (HAT0X)  hy <...>  dpad Y
#   ax <0..255>      stick X (ABS_X)      ay <0..255>  stick Y
#   q                quit
# codes evdev: 288 BTN_TRIGGER .. 299. mapping SDL(0810:0001): b0=288 b1=289 b2=290 b3=291
import os, struct, fcntl, time, sys, select
UINPUT="/dev/uinput"; FIFO="/tmp/vpadcmd"
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
    fd=create()
    for a in (0,1,2,5): emit(fd,EV_ABS,a,128 if a in(2,5) else 127)
    emit(fd,EV_ABS,16,0); emit(fd,EV_ABS,17,0); syn(fd)
    if os.path.exists(FIFO): os.unlink(FIFO)
    os.mkfifo(FIFO)
    ff=os.open(FIFO,os.O_RDONLY|os.O_NONBLOCK)
    sys.stderr.write("[VPADD] ready, fifo=%s\n"%FIFO); sys.stderr.flush()
    buf=b""; deadline=time.time()+120  # auto-quit em 120s
    while time.time()<deadline:
        r,_,_=select.select([ff],[],[],0.2)
        if ff in r:
            try: chunk=os.read(ff,4096)
            except OSError: chunk=b""
            if not chunk:
                os.close(ff); ff=os.open(FIFO,os.O_RDONLY|os.O_NONBLOCK); continue
            buf+=chunk
            while b"\n" in buf:
                line,buf=buf.split(b"\n",1)
                s=line.decode(errors="ignore").strip()
                if not s: continue
                p=s.split()
                try:
                    if p[0]=="q": raise SystemExit
                    elif p[0]=="p": emit(fd,EV_KEY,int(p[1]),1); syn(fd)
                    elif p[0]=="r": emit(fd,EV_KEY,int(p[1]),0); syn(fd)
                    elif p[0]=="t":
                        ms=int(p[2]) if len(p)>2 else 120
                        emit(fd,EV_KEY,int(p[1]),1); syn(fd); time.sleep(ms/1000.0)
                        emit(fd,EV_KEY,int(p[1]),0); syn(fd)
                    elif p[0]=="hx": emit(fd,EV_ABS,16,int(p[1])); syn(fd)
                    elif p[0]=="hy": emit(fd,EV_ABS,17,int(p[1])); syn(fd)
                    elif p[0]=="ax": emit(fd,EV_ABS,0,int(p[1])); syn(fd)
                    elif p[0]=="ay": emit(fd,EV_ABS,1,int(p[1])); syn(fd)
                    deadline=time.time()+120
                    sys.stderr.write("[VPADD] %s\n"%s); sys.stderr.flush()
                except SystemExit: raise
                except Exception as e:
                    sys.stderr.write("[VPADD] err %s: %s\n"%(s,e)); sys.stderr.flush()
    try: fcntl.ioctl(fd,UI_DEV_DESTROY)
    except Exception: pass
    os.close(fd)
if __name__=="__main__":
    try: main()
    except SystemExit: pass
