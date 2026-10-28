"""Browser input to game-local raw-input bridge, without OS cursor warping."""
import ctypes
from ctypes import wintypes as W
import threading
u=ctypes.WinDLL('user32',use_last_error=True)
class RECT(ctypes.Structure): _fields_=[('left',W.LONG),('top',W.LONG),('right',W.LONG),('bottom',W.LONG)]
class PACKET(ctypes.Structure): _fields_=[('version',W.DWORD),('type',W.DWORD),('x',W.LONG),('y',W.LONG),('dx',W.LONG),('dy',W.LONG),('button',W.DWORD),('down',W.DWORD),('vk',W.DWORD),('scan',W.DWORD),('wheel',W.LONG)]
class COPY(ctypes.Structure): _fields_=[('tag',ctypes.c_size_t),('bytes',W.DWORD),('data',ctypes.c_void_p)]
assert ctypes.sizeof(PACKET)==44
u.GetWindowThreadProcessId.argtypes=[W.HWND,ctypes.POINTER(W.DWORD)]
u.GetClientRect.argtypes=[W.HWND,ctypes.POINTER(RECT)]
u.SendMessageTimeoutW.argtypes=[W.HWND,W.UINT,W.WPARAM,W.LPARAM,W.UINT,W.UINT,ctypes.POINTER(ctypes.c_size_t)]
u.SendMessageTimeoutW.restype=W.LPARAM
keys={'Enter':0x0d,'Escape':0x1b,'ArrowLeft':0x25,'ArrowUp':0x26,'ArrowRight':0x27,'ArrowDown':0x28,'Space':0x20,'Tab':0x09,'Backspace':0x08,
      **{f'Key{chr(i)}':i for i in range(65,91)},**{f'Digit{i}':48+i for i in range(10)},'ShiftLeft':0xa0,'ShiftRight':0xa1,'ControlLeft':0xa2,'ControlRight':0xa3}
lock=threading.Lock()
point=None
def route(window,pid,event):
    global point
    kind=event.get('type')
    if kind not in ('key','move','button','wheel'): raise ValueError('Unsupported input event')
    if kind=='key' and event.get('code') not in keys: raise ValueError('Unsupported key')
    if kind=='button' and event.get('button') not in (0,1,2): raise ValueError('Unsupported mouse button')
    with lock:
        actual=W.DWORD();u.GetWindowThreadProcessId(window,ctypes.byref(actual))
        if actual.value!=pid: raise RuntimeError('Captured game window no longer belongs to game PID')
        rect=RECT();u.GetClientRect(window,ctypes.byref(rect))
        previous=point or (rect.right//2,rect.bottom//2)
        if kind!='key':
            x=float(event.get('x',-1));y=float(event.get('y',-1))
            if not (0<=x<=1 and 0<=y<=1): raise ValueError('Input outside image')
            point=(round(x*(rect.right-1)),round(y*(rect.bottom-1)))
        else:point=previous
        packet=PACKET(1,{'move':1,'button':2,'wheel':3,'key':4}[kind],*point,
                      point[0]-previous[0],point[1]-previous[1],int(event.get('button',0)),
                      int(bool(event.get('down',False))),0,0,0)
        if kind=='key':packet.vk=keys[event['code']];packet.scan=u.MapVirtualKeyW(packet.vk,0)
        if kind=='wheel':packet.wheel=max(-1200,min(1200,int(event.get('delta',0))))
        data=COPY(0x43504249,ctypes.sizeof(packet),ctypes.addressof(packet))
        result=ctypes.c_size_t()
        sent=u.SendMessageTimeoutW(window,0x4a,0,ctypes.addressof(data),2,1000,ctypes.byref(result))
        if not sent or not result.value: raise RuntimeError('Game input bridge did not accept event')
    return {'accepted':True,'mode':'original game raw input','cursorWarped':False,'focusChanged':False,'gamePoint':point}
