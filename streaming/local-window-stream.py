"""Loopback-only, live capture of one specified Windows game process.

This streams the native game window; it does not run the engine in WebAssembly.
It never captures the desktop or substitutes video when capture fails.
"""
import argparse
import ctypes
import json
import subprocess
import threading
import time
import importlib.util
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

parser=argparse.ArgumentParser()
parser.add_argument('--pid',type=int,required=True)
parser.add_argument('--port',type=int,default=8079)
args=parser.parse_args()
input_spec=importlib.util.spec_from_file_location('native_game_input',Path(__file__).with_name('native-game-input.py'))
menu_input=importlib.util.module_from_spec(input_spec);input_spec.loader.exec_module(menu_input)
user32=ctypes.WinDLL('user32',use_last_error=True)
user32.IsWindow.argtypes=[ctypes.c_void_p]
user32.GetWindowThreadProcessId.argtypes=[ctypes.c_void_p,ctypes.POINTER(ctypes.c_ulong)]
user32.IsWindowVisible.argtypes=[ctypes.c_void_p]
hwnd=None
callback_type=ctypes.WINFUNCTYPE(ctypes.c_bool,ctypes.c_void_p,ctypes.c_void_p)
def inspect_window(window,_):
    global hwnd
    pid=ctypes.c_ulong()
    user32.GetWindowThreadProcessId(window,ctypes.byref(pid))
    if pid.value==args.pid and user32.IsWindowVisible(window):
        hwnd=window
        return False
    return True
callback=callback_type(inspect_window)
for attempt in range(60):
    user32.EnumWindows(callback,None)
    if hwnd: break
    time.sleep(0.5)
if not hwnd: raise SystemExit('No visible window for the requested game PID')
ffmpeg=Path(r'C:\Users\Brady\AppData\Local\universal-modder\ffmpeg\bin\ffmpeg.exe')
log=Path(__file__).with_name('capture.log').open('wb')
command=[str(ffmpeg),'-hide_banner','-loglevel','warning','-f','lavfi','-i',
         f'gfxcapture=hwnd={hwnd}:capture_cursor=0:display_border=0:max_framerate=30',
         '-vf',"hwdownload,format=bgra,scale=w='min(1280,iw)':h=-2,format=yuvj420p",'-an','-c:v','mjpeg','-q:v','3',
         '-f','image2pipe','pipe:1']
process=subprocess.Popen(command,stdout=subprocess.PIPE,stderr=log,creationflags=subprocess.CREATE_NO_WINDOW)
condition=threading.Condition()
frame=b''
sequence=0
last_frame=0.0
capture_error=None
def capture():
    global frame,sequence,last_frame,capture_error
    pending=bytearray()
    try:
        while True:
            data=process.stdout.read1(65536)
            if not data: break
            pending.extend(data)
            while True:
                start=pending.find(b'\xff\xd8')
                if start<0:
                    pending[:]=pending[-1:]
                    break
                end=pending.find(b'\xff\xd9',start+2)
                if end<0:
                    del pending[:start]
                    break
                image=bytes(pending[start:end+2])
                del pending[:end+2]
                with condition:
                    frame=image;sequence+=1;last_frame=time.time()
                    condition.notify_all()
        capture_error=f'Capture ended, ffmpeg exit={process.wait()}'
    except Exception as exc: capture_error=str(exc)
    finally:
        with condition: condition.notify_all()
threading.Thread(target=capture,daemon=True).start()
class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.headers.get('Host') not in (f'127.0.0.1:{args.port}',f'localhost:{args.port}'):
            self.send_error(403);return
        if self.path=='/':
            data=Path(__file__).with_name('index.html').read_bytes()
            self.send_response(200);self.send_header('Content-Type','text/html; charset=utf-8')
            self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
            self.end_headers();self.wfile.write(data)
        elif self.path=='/status':
            data=json.dumps({'mode':'native Windows window stream','gamePID':args.pid,
                 'hwnd':hwnd,'frames':sequence,'frameAgeSeconds':round(time.time()-last_frame,3) if last_frame else None,
                 'captureError':capture_error,'audio':False,'browserInput':'game raw input bridge: continuous mouse, buttons, wheel and keyboard'}).encode()
            self.send_response(200);self.send_header('Content-Type','application/json')
            self.send_header('Content-Length',str(len(data)));self.send_header('Cache-Control','no-store')
            self.end_headers();self.wfile.write(data)
        elif self.path=='/stream.mjpg':
            self.send_response(200)
            self.send_header('Content-Type','multipart/x-mixed-replace; boundary=gameframe')
            self.send_header('Cache-Control','no-store');self.end_headers()
            previous=-1
            try:
                while True:
                    with condition:
                        condition.wait_for(lambda:sequence!=previous or capture_error,timeout=5)
                        if capture_error: return
                        if sequence==previous or not frame: continue
                        current=frame;previous=sequence
                    self.wfile.write(b'--gameframe\r\nContent-Type: image/jpeg\r\nContent-Length: '+str(len(current)).encode()+b'\r\n\r\n'+current+b'\r\n')
                    self.wfile.flush()
            except (BrokenPipeError,ConnectionResetError): pass
        else: self.send_error(404)
    def do_POST(self):
        allowed=(f'http://127.0.0.1:{args.port}',f'http://localhost:{args.port}')
        if self.path!='/input' or self.headers.get('Origin') not in allowed or self.headers.get('Host') not in (f'127.0.0.1:{args.port}',f'localhost:{args.port}'):
            self.send_error(403);return
        length=int(self.headers.get('Content-Length','0'))
        if not 0<length<=1024: self.send_error(400);return
        try:
            result=menu_input.route(hwnd,args.pid,json.loads(self.rfile.read(length)))
            code=200
        except Exception as error:
            result={'accepted':False,'error':str(error)};code=400
        data=json.dumps(result).encode()
        self.send_response(code);self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(data)));self.end_headers();self.wfile.write(data)
    def log_message(self,format,*values): pass
server=ThreadingHTTPServer(('127.0.0.1',args.port),Handler)
print(json.dumps({'url':f'http://127.0.0.1:{args.port}/','gamePID':args.pid,'hwnd':hwnd,'capturePID':process.pid}),flush=True)
try: server.serve_forever()
finally:
    process.terminate();server.server_close();log.close()
