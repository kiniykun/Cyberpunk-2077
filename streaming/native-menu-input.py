"""Discrete menu input to the captured game, restoring the browser afterwards."""
import ctypes
from ctypes import wintypes as W
import threading
import time

u=ctypes.WinDLL('user32',use_last_error=True)
k=ctypes.WinDLL('kernel32',use_last_error=True)
class POINT(ctypes.Structure): _fields_=[('x',W.LONG),('y',W.LONG)]
class RECT(ctypes.Structure): _fields_=[('left',W.LONG),('top',W.LONG),('right',W.LONG),('bottom',W.LONG)]
class MOUSE(ctypes.Structure): _fields_=[('dx',W.LONG),('dy',W.LONG),('data',W.DWORD),('flags',W.DWORD),('time',W.DWORD),('extra',ctypes.c_size_t)]
class KEY(ctypes.Structure): _fields_=[('vk',W.WORD),('scan',W.WORD),('flags',W.DWORD),('time',W.DWORD),('extra',ctypes.c_size_t)]
class UNION(ctypes.Union): _fields_=[('mouse',MOUSE),('key',KEY)]
class INPUT(ctypes.Structure): _fields_=[('type',W.DWORD),('value',UNION)]
u.GetForegroundWindow.restype=W.HWND
u.GetWindowThreadProcessId.argtypes=[W.HWND,ctypes.POINTER(W.DWORD)]
u.GetWindowThreadProcessId.restype=W.DWORD
u.SetForegroundWindow.argtypes=[W.HWND]
u.AttachThreadInput.argtypes=[W.DWORD,W.DWORD,W.BOOL]
u.SendInput.argtypes=[W.UINT,ctypes.POINTER(INPUT),ctypes.c_int]
u.GetClientRect.argtypes=[W.HWND,ctypes.POINTER(RECT)]
u.ClientToScreen.argtypes=[W.HWND,ctypes.POINTER(POINT)]
u.GetCursorPos.argtypes=[ctypes.POINTER(POINT)]
u.SetCursorPos.argtypes=[ctypes.c_int,ctypes.c_int]
keys={'Enter':0x0d,'Escape':0x1b,'ArrowLeft':0x25,'ArrowUp':0x26,'ArrowRight':0x27,'ArrowDown':0x28,'Space':0x20,'Tab':0x09,'Backspace':0x08}
lock=threading.Lock()
def route(window,pid,event):
    if event.get('type') not in ('key','click'): raise ValueError('Unsupported menu event')
    if event['type']=='key' and event.get('code') not in keys: raise ValueError('Unsupported menu key')
    x=float(event.get('x',0));y=float(event.get('y',0))
    if event['type']=='click' and not (0<=x<=1 and 0<=y<=1): raise ValueError('Click outside game image')
    with lock:
        actual=W.DWORD()
        game_thread=u.GetWindowThreadProcessId(window,ctypes.byref(actual))
        if actual.value!=pid: raise RuntimeError('Captured game window no longer belongs to game PID')
        previous=u.GetForegroundWindow()
        previous_thread=u.GetWindowThreadProcessId(previous,None) if previous else 0
        current=k.GetCurrentThreadId()
        attached=[]
        cursor=POINT();u.GetCursorPos(ctypes.byref(cursor))
        try:
            for thread in {previous_thread,game_thread}-{0,current}:
                if u.AttachThreadInput(current,thread,True): attached.append(thread)
            u.SetForegroundWindow(window)
            if u.GetForegroundWindow()!=window: raise RuntimeError('Game could not receive foreground input')
            if event['type']=='key':
                vk=keys[event['code']];scan=u.MapVirtualKeyW(vk,0)
                extended=1 if event['code'].startswith('Arrow') else 0
                down=INPUT(1,UNION(key=KEY(0,scan,8|extended,0,0)))
                up=INPUT(1,UNION(key=KEY(0,scan,8|2|extended,0,0)))
            else:
                rect=RECT();u.GetClientRect(window,ctypes.byref(rect))
                point=POINT(int(x*(rect.right-rect.left-1)),int(y*(rect.bottom-rect.top-1)))
                u.ClientToScreen(window,ctypes.byref(point));u.SetCursorPos(point.x,point.y)
                down=INPUT(0,UNION(mouse=MOUSE(0,0,0,2,0,0)))
                up=INPUT(0,UNION(mouse=MOUSE(0,0,0,4,0,0)))
            if u.SendInput(1,ctypes.byref(down),ctypes.sizeof(INPUT))!=1: raise RuntimeError('Game input injection failed')
            time.sleep(0.06)
            if u.GetForegroundWindow()!=window: raise RuntimeError('Game lost foreground during input')
            if u.SendInput(1,ctypes.byref(up),ctypes.sizeof(INPUT))!=1: raise RuntimeError('Game input release failed')
            time.sleep(0.03)
        finally:
            if previous and u.GetForegroundWindow()==window: u.SetForegroundWindow(previous)
            if event['type']=='click': u.SetCursorPos(cursor.x,cursor.y)
            for thread in attached: u.AttachThreadInput(current,thread,False)
    return {'accepted':True,'mode':'discrete native menu input'}
