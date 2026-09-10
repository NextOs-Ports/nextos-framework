#!/usr/bin/env python3
# navkbd.py - SoR4 le TECLADO (via gptokeyb: a=space, start=enter, b=ctrl, dpad=setas).
# Cria um TECLADO virtual via uinput e martela START(enter)+A(space) alternado p/ avancar
# o menu ate a gameplay. Teclado NAO tem problema de player-index (id), o jogo le qualquer
# teclado via SDL. Roda 'cycles' e sai sozinho (destroi o device).
# uso: navkbd.py <cycles> [period_ms]   (default cycles=40, period=500ms)
import os, struct, fcntl, time, sys
UINPUT="/dev/uinput"
EV_SYN,EV_KEY=0,1
def _IOC(d,t,nr,sz): return (d<<30)|(sz<<16)|(t<<8)|nr
UI_SET_EVBIT=_IOC(1,ord('U'),100,4); UI_SET_KEYBIT=_IOC(1,ord('U'),101,4)
UI_DEV_CREATE=_IOC(0,ord('U'),1,0); UI_DEV_DESTROY=_IOC(0,ord('U'),2,0)
# evdev key codes: esc1 enter28 lctrl29 lshift42 lalt56 space57 up103 left105 right106 down108
KEYS=[1,28,29,42,56,57,103,105,106,108]
ENTER=28; SPACE=57
def create():
    fd=os.open(UINPUT,os.O_WRONLY|os.O_NONBLOCK)
    fcntl.ioctl(fd,UI_SET_EVBIT,EV_KEY); fcntl.ioctl(fd,UI_SET_EVBIT,EV_SYN)
    for k in KEYS: fcntl.ioctl(fd,UI_SET_KEYBIT,k)
    name=b"sor4-vkbd"; name=name+b"\x00"*(80-len(name))
    blob=name+struct.pack("HHHH",0x0003,0x1234,0x5678,0x0001)+struct.pack("I",0)
    blob+=struct.pack("64i",*([0]*64))+struct.pack("64i",*([0]*64))+struct.pack("64i",*([0]*64))+struct.pack("64i",*([0]*64))
    os.write(fd,blob); fcntl.ioctl(fd,UI_DEV_CREATE); return fd
def ev(fd,t,c,v): os.write(fd,struct.pack("llHHi",0,0,t,c,v))
def tap(fd,k):
    ev(fd,EV_KEY,k,1); ev(fd,EV_SYN,0,0); time.sleep(0.06)
    ev(fd,EV_KEY,k,0); ev(fd,EV_SYN,0,0)
def main():
    cycles=int(sys.argv[1]) if len(sys.argv)>1 else 40
    per=(int(sys.argv[2]) if len(sys.argv)>2 else 500)/1000.0
    fd=create()
    time.sleep(1.0)  # deixa o SDL/udev hotplug enxergar o teclado
    sys.stderr.write("[NAVKBD] teclado virtual criado, martelando ENTER+SPACE\n"); sys.stderr.flush()
    try:
        for i in range(cycles):
            tap(fd,ENTER); time.sleep(per)   # Start
            tap(fd,SPACE); time.sleep(per)   # A
            sys.stderr.write("[NAVKBD] cycle %d enter+space\n"%(i+1)); sys.stderr.flush()
    finally:
        try: fcntl.ioctl(fd,UI_DEV_DESTROY)
        except Exception: pass
        os.close(fd)
    sys.stderr.write("[NAVKBD] fim\n"); sys.stderr.flush()
if __name__=="__main__":
    try: main()
    except (KeyboardInterrupt,SystemExit): pass
