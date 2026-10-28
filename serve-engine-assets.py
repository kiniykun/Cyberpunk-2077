"""Serve original game assets and the compiled browser runtime without copying assets.

This is the transport for the engine's future browser filesystem integration.
Serving assets alone does not launch or render the game.
"""
import argparse
import json
import mimetypes
import os
import re
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import unquote, urlsplit

PORT_ROOT = Path(__file__).resolve().parent
DEFAULT_DEPOT = Path(r"D:\cp2077SourceCode\extracted\root\R6.Root\Mainline")


def accepts_gzip(header):
    values = {}
    for item in (header or '').split(','):
        parts = item.strip().lower().split(';')
        quality = 1.0
        for part in parts[1:]:
            if part.strip().startswith('q='):
                try:
                    quality = float(part.strip()[2:])
                except ValueError:
                    quality = 0.0
        values[parts[0].strip()] = quality
    return 0.0 < values.get('gzip', values.get('*', 0.0)) <= 1.0


def selected_range(header, size):
    """Return an inclusive single range, or reject an unsatisfiable request."""
    if header is None:
        return 0, size - 1, False
    match = re.fullmatch(r"bytes=(\d*)-(\d*)", header.strip())
    if not match or size == 0:
        raise ValueError("Invalid or unsupported byte range")
    first, last = match.groups()
    if not first:
        count = int(last or "0")
        if count <= 0:
            raise ValueError("Invalid suffix length")
        return max(0, size - count), size - 1, True
    start = int(first)
    end = min(int(last), size - 1) if last else size - 1
    if start >= size or end < start:
        raise ValueError("Unsatisfiable byte range")
    return start, end, True


class AssetHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("X-Content-Type-Options", "nosniff")
        super().end_headers()

    def do_HEAD(self):
        if urlsplit(self.path).path == '/runtime/game-files.json':
            self.serve_manifest(False)
            return
        self.serve_file(False)

    def do_GET(self):
        if urlsplit(self.path).path == '/runtime/game-files.json':
            self.serve_manifest(True)
            return
        self.serve_file(True)

    @staticmethod
    def etag(stat):
        return '"%x-%x"' % (stat.st_size, stat.st_mtime_ns)

    def serve_manifest(self, send_body):
        entries = []
        archive_root = PORT_ROOT / 'archive' / 'pc'
        if archive_root.is_dir():
            for target in sorted(archive_root.rglob('*')):
                if target.is_file() and target.suffix in ('.archive', '.bin', '.cache', '.h64'):
                    relative = target.relative_to(archive_root).as_posix()
                    stat = target.stat()
                    entries.append({'path': '/game/archive/pc/' + relative,
                                    'size': stat.st_size, 'mtime': stat.st_mtime * 1000,
                                    'etag': self.etag(stat)})
        for filename, directory in [('master.redscripts', PORT_ROOT / 'cook-lab/r6/cache'),
                                    ('final.redscripts', PORT_ROOT / 'cook-lab/r6/cache'),
                                    ('tweakdb.bin', PORT_ROOT / 'cooked/pc/cache')]:
            target = directory / filename
            if target.is_file():
                stat = target.stat()
                entries.append({'path': '/game/r6/cache/' + filename,
                                'size': stat.st_size, 'mtime': stat.st_mtime * 1000,
                                'etag': self.etag(stat)})
        # The original final-build shader resolver reads this engine-root file.
        target = self.server.depot / 'engine/shader_final.cache'
        if target.is_file():
            stat = target.stat()
            entries.append({'path': '/game/engine/shader_final.cache',
                            'size': stat.st_size, 'mtime': stat.st_mtime * 1000,
                            'etag': self.etag(stat)})
        body = json.dumps({'version': 1, 'files': entries}).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if send_body:
            self.wfile.write(body)

    def resolve_file(self):
        route = unquote(urlsplit(self.path).path)
        if "\x00" in route or "\\" in route or ":" in route:
            return None
        parts = route.split("/")
        if any(part in (".", "..") for part in parts):
            return None
        if route in ("/", "/native-browser.html"):
            return PORT_ROOT / "native-browser.html"
        if route == "/sdk/bink2w64.dll":
            # One explicitly selected codec binary remains in the original
            # local depot. Do not expose dev/source or a general SDK directory.
            candidate = self.server.depot / "dev/external/bink2/lib/x64_DLL.Release/bink2w64.dll"
            return candidate if candidate.is_file() else None
        if route.startswith('/game/archive/pc/'):
            root = (PORT_ROOT / 'archive/pc').resolve()
            candidate = (root / route[len('/game/archive/pc/'):]).resolve()
            if candidate.is_relative_to(root) and candidate.is_file():
                return candidate
            return None
        if route in ('/game/r6/cache/master.redscripts', '/game/r6/cache/final.redscripts', '/game/r6/cache/tweakdb.bin'):
            directory = 'cook-lab/r6/cache' if route.endswith('.redscripts') else 'cooked/pc/cache'
            candidate = PORT_ROOT / directory / route.rsplit('/', 1)[1]
            return candidate if candidate.is_file() else None
        if route.startswith("/runtime/"):
            root = PORT_ROOT / "build" / "game"
            relative = route[len("/runtime/"):]
        elif route.startswith("/game/"):
            relative = route[len("/game/"):]
            # Only resource directories are exposed, never dev/source or the D: root.
            if relative.split("/", 1)[0].lower() not in ("archive", "engine", "r6"):
                return None
            root = self.server.depot
        else:
            return None
        root = root.resolve()
        candidate = (root / relative).resolve()
        if not candidate.is_relative_to(root) or not candidate.is_file():
            return None
        return candidate

    def serve_file(self, send_body):
        try:
            target = self.resolve_file()
            if target is None:
                self.send_error(404, "No asset or compiled runtime at this path")
                return
            # Prepared compression applies only to immutable engine bundles.
            # Asset range offsets and validators always refer to identity bytes.
            immutable = bool(re.fullmatch(r'/runtime/engine-versions/[0-9a-f]{64}/cp2077\.(wasm|js|data)', urlsplit(self.path).path))
            compressed = target.with_name(target.name + '.gz')
            use_gzip = immutable and self.headers.get('Range') is None and accepts_gzip(self.headers.get('Accept-Encoding')) and compressed.is_file()
            # Open once: file length and transferred bytes belong to the same handle.
            with (compressed if use_gzip else target).open("rb") as stream:
                stat = os.fstat(stream.fileno())
                size = stat.st_size
                etag = self.etag(stat)
                if self.headers.get('If-Match') not in (None, etag):
                    self.send_response(412)
                    self.send_header('Content-Length', '0')
                    self.end_headers()
                    return
                try:
                    first, last, partial = selected_range(self.headers.get("Range"), size)
                except ValueError:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                length = max(0, last - first + 1)
                self.send_response(206 if partial else 200)
                content_type = "application/wasm" if target.suffix == ".wasm" else mimetypes.guess_type(target.name)[0]
                self.send_header("Content-Type", content_type or "application/octet-stream")
                self.send_header("Accept-Ranges", "bytes")
                self.send_header('ETag', etag)
                self.send_header("Cache-Control", "public, max-age=31536000, immutable" if immutable else "no-cache")
                if immutable:
                    self.send_header('Vary', 'Accept-Encoding')
                if use_gzip:
                    self.send_header('Content-Encoding', 'gzip')
                self.send_header("Content-Length", str(length))
                if partial:
                    self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
                self.end_headers()
                if send_body:
                    stream.seek(first)
                    remaining = length
                    while remaining:
                        block = stream.read(min(256 * 1024, remaining))
                        if not block:
                            self.close_connection = True
                            break
                        self.wfile.write(block)
                        remaining -= len(block)
        except (BrokenPipeError, ConnectionResetError):
            self.close_connection = True
        except OSError:
            # Avoid returning local paths or switching status after headers were sent.
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--depot", type=Path, default=DEFAULT_DEPOT)
    parser.add_argument("--port", type=int, default=8078)
    args = parser.parse_args()
    depot = args.depot.resolve(strict=True)
    for directory in ("archive", "engine", "r6"):
        if not (depot / directory).is_dir():
            parser.error(f"Game resource directory missing: {directory}")
    with ThreadingHTTPServer(("127.0.0.1", args.port), AssetHandler) as server:
        server.depot = depot
        print(f"Original asset transport: http://127.0.0.1:{args.port}/game/", flush=True)
        print("Compiled engine files: /runtime/; browser filesystem and launcher integration are pending.", flush=True)
        server.serve_forever()


if __name__ == "__main__":
    main()
