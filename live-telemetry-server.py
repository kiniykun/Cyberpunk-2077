"""Loopback-only bounded browser telemetry, independent of asset serving."""
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
from pathlib import Path
import json,threading,time
root=Path(__file__).resolve().parent/'build/live-telemetry'
root.mkdir(parents=True,exist_ok=True)
lock=threading.Lock();latest=None
allowed={'http://127.0.0.1:8078','http://localhost:8078'}
class Handler(BaseHTTPRequestHandler):
    def log_message(self,*args):pass
    def reply(self,status,body=b''):
        self.send_response(status)
        origin=self.headers.get('Origin')
        if origin in allowed:self.send_header('Access-Control-Allow-Origin',origin)
        self.send_header('Vary','Origin')
        self.send_header('Access-Control-Allow-Methods','POST, GET, OPTIONS')
        self.send_header('Access-Control-Allow-Headers','Content-Type')
        self.send_header('Cross-Origin-Resource-Policy','cross-origin')
        self.send_header('Cache-Control','no-store')
        self.send_header('Content-Type','application/json')
        self.send_header('Content-Length',str(len(body)));self.end_headers()
        if body:self.wfile.write(body)
    def do_OPTIONS(self):self.reply(204 if self.headers.get('Origin') in allowed else 403)
    def do_GET(self):
        if self.path!='/telemetry/latest':self.reply(404);return
        if self.headers.get('Origin') not in allowed|{None}:self.reply(403);return
        with lock:body=json.dumps(latest).encode()
        self.reply(200,body)
    def do_POST(self):
        global latest
        if self.path.startswith('/dump/') and self.headers.get('Origin') in allowed:
            # Diagnostic raw byte dumps (e.g. texture mips) written to build/live-telemetry/dump-<name>.bin.
            name=''.join(c for c in self.path[6:] if c.isalnum() or c in '-_x')[:64]
            size=int(self.headers.get('Content-Length','0'))
            if not name or not 0<size<=64*1024*1024:self.reply(400);return
            (root/('dump-'+name+'.bin')).write_bytes(self.rfile.read(size));self.reply(204);return
        if self.path not in {'/telemetry','/trace'} or self.headers.get('Origin') not in allowed:self.reply(403);return
        try:
            size=int(self.headers.get('Content-Length','0'))
            if not 0<size<=(32*1024*1024 if self.path=='/trace' else 65536):raise ValueError('Invalid size')
            record=json.loads(self.rfile.read(size))
            if self.path=='/trace':
                if not isinstance(record,dict) or not isinstance(record.get('records'),list) or len(record['records'])>524288:raise ValueError('Invalid trace')
                with lock:
                    temporary=root/'native-trace.tmp';temporary.write_text(json.dumps(record,separators=(',',':')),encoding='utf8');temporary.replace(root/'native-trace.json')
                self.reply(204);return
            if not isinstance(record,dict) or record.get('version')!=1:raise ValueError('Invalid record')
            record['serverReceivedAt']=time.time()
            body=json.dumps(record,separators=(',',':')).encode()
            with lock:
                latest=record
                temporary=root/'latest.tmp';temporary.write_bytes(body);temporary.replace(root/'latest.json')
                history=root/'history.jsonl'
                if history.exists() and history.stat().st_size>10*1024*1024:history.replace(root/'history.previous.jsonl')
                with history.open('ab') as stream:stream.write(body+b'\n')
            self.reply(204)
        except (ValueError,TypeError,json.JSONDecodeError):self.reply(400)
ThreadingHTTPServer(('127.0.0.1',8091),Handler).serve_forever()
