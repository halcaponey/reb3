#!/usr/bin/env python3
"""webserve.py -- the dev server for the Burnout 3 RE web shell.

Stdlib only, no dependencies, no build step.  It exists because a plain
`python3 -m http.server` cannot host this page: the Emscripten engine is built
with PROXY_TO_PTHREAD, which needs SharedArrayBuffer, which the browser only
hands out to a *cross-origin isolated* document.  That isolation is granted by
two response headers, and they have to be on EVERY response:

    Cross-Origin-Opener-Policy:   same-origin
    Cross-Origin-Embedder-Policy: require-corp

Without them `crossOriginIsolated` is false, SharedArrayBuffer is undefined,
and the pthread build refuses to start.  The landing page shows both as badges
so a misconfigured server is visible at a glance.

On top of that:
  * correct MIME for .wasm (application/wasm) -- Chrome refuses to
    streaming-compile anything else;
  * Range requests, so a big .data package or a served disc image can be
    fetched in pieces;
  * no-store, so a rebuilt burnout3.wasm is never served stale.

Usage:
    python3 tools/webserve.py                 # repo root, port 8080
    python3 tools/webserve.py --port 9000
    python3 tools/webserve.py --root web --port 9000     # web/ as doc root
"""

import argparse
import http.server
import os
import posixpath
import socket
import socketserver
import sys
import urllib.parse

# Emscripten output, page assets, and the odd asset pack the engine may emit.
MIME = {
    '.wasm': 'application/wasm',
    '.js':   'text/javascript',
    '.mjs':  'text/javascript',
    '.cjs':  'text/javascript',
    '.json': 'application/json',
    '.map':  'application/json',
    '.css':  'text/css',
    '.html': 'text/html',
    '.htm':  'text/html',
    '.svg':  'image/svg+xml',
    '.png':  'image/png',
    '.jpg':  'image/jpeg',
    '.webp': 'image/webp',
    '.ico':  'image/x-icon',
    '.woff2': 'font/woff2',
    '.data': 'application/octet-stream',
    '.iso':  'application/octet-stream',
    '.bin':  'application/octet-stream',
    '.txt':  'text/plain',
}

ISOLATION_HEADERS = (
    ('Cross-Origin-Opener-Policy', 'same-origin'),
    ('Cross-Origin-Embedder-Policy', 'require-corp'),
    # Same-origin subresources are fine under require-corp, but being explicit
    # keeps a reverse proxy or a second origin from silently breaking it.
    ('Cross-Origin-Resource-Policy', 'same-origin'),
)


class Handler(http.server.SimpleHTTPRequestHandler):
    server_version = 'b3webserve/1.0'
    protocol_version = 'HTTP/1.1'

    extensions_map = dict(http.server.SimpleHTTPRequestHandler.extensions_map)
    extensions_map.update(MIME)

    # ------------------------------------------------------------- headers
    def end_headers(self):
        for k, v in ISOLATION_HEADERS:
            self.send_header(k, v)
        # A dev server that caches is a dev server that lies about your build.
        self.send_header('Cache-Control', 'no-store, must-revalidate')
        http.server.SimpleHTTPRequestHandler.end_headers(self)

    # ---------------------------------------------------------- convenience
    def do_GET(self):
        if self.path in ('/', '/index.html') and self.landing_redirect():
            return
        http.server.SimpleHTTPRequestHandler.do_GET(self)

    def do_HEAD(self):
        if self.path in ('/', '/index.html') and self.landing_redirect():
            return
        http.server.SimpleHTTPRequestHandler.do_HEAD(self)

    def landing_redirect(self):
        """Serving the repo root?  Then / should land on the shell, not on a
        directory listing of the whole checkout."""
        root = getattr(self.server, 'doc_root', '.')
        if os.path.isfile(os.path.join(root, 'index.html')):
            return False
        if not os.path.isfile(os.path.join(root, 'web', 'index.html')):
            return False
        self.send_response(302)
        self.send_header('Location', '/web/')
        self.send_header('Content-Length', '0')
        self.end_headers()
        return True

    # --------------------------------------------------------------- ranges
    def send_head(self):
        self._range = None
        rng = self.headers.get('Range')
        if not rng:
            return http.server.SimpleHTTPRequestHandler.send_head(self)

        path = self.translate_path(self.path)
        if os.path.isdir(path) or not os.path.isfile(path):
            return http.server.SimpleHTTPRequestHandler.send_head(self)

        size = os.path.getsize(path)
        span = parse_range(rng, size)
        if span is None:
            self.send_response(416)
            self.send_header('Content-Range', 'bytes */%d' % size)
            self.send_header('Content-Length', '0')
            self.end_headers()
            return None

        start, end = span
        try:
            f = open(path, 'rb')
        except OSError:
            self.send_error(404, 'File not found')
            return None
        f.seek(start)
        self._range = end - start + 1

        self.send_response(206)
        self.send_header('Content-Type', self.guess_type(path))
        self.send_header('Accept-Ranges', 'bytes')
        self.send_header('Content-Range', 'bytes %d-%d/%d' % (start, end, size))
        self.send_header('Content-Length', str(self._range))
        self.send_header('Last-Modified', self.date_time_string(os.path.getmtime(path)))
        self.end_headers()
        return f

    def copyfile(self, source, outputfile):
        remaining = getattr(self, '_range', None)
        if remaining is None:
            return http.server.SimpleHTTPRequestHandler.copyfile(self, source, outputfile)
        while remaining > 0:
            chunk = source.read(min(64 * 1024, remaining))
            if not chunk:
                break
            outputfile.write(chunk)
            remaining -= len(chunk)

    # Advertise range support on ordinary responses too.
    def send_response(self, code, message=None):
        http.server.SimpleHTTPRequestHandler.send_response(self, code, message)
        if code == 200:
            self.send_header('Accept-Ranges', 'bytes')

    # ------------------------------------------------------------- quieter
    def log_message(self, fmt, *args):
        if getattr(self.server, 'quiet', False):
            return
        sys.stderr.write('  %s  %s\n' % (self.log_date_time_string(), fmt % args))

    def translate_path(self, path):
        """Same as the stock one, but rooted at --root instead of cwd."""
        path = path.split('?', 1)[0].split('#', 1)[0]
        trailing = path.rstrip().endswith('/')
        path = posixpath.normpath(urllib.parse.unquote(path))
        words = [w for w in path.split('/') if w and w not in ('.', '..')]
        out = getattr(self.server, 'doc_root', os.getcwd())
        for w in words:
            drive, w = os.path.splitdrive(w)
            head, w = os.path.split(w)
            if w in (os.curdir, os.pardir):
                continue
            out = os.path.join(out, w)
        if trailing:
            out += '/'
        return out


def parse_range(header, size):
    """Single `bytes=a-b` span only -- that is all a browser ever sends for a
    media or wasm fetch.  Returns (start, end) inclusive, or None for 416."""
    if not header.startswith('bytes='):
        return None
    spec = header[6:].split(',')[0].strip()
    if '-' not in spec:
        return None
    a, _, b = spec.partition('-')
    try:
        if not a:                       # suffix form: bytes=-500
            n = int(b)
            if n <= 0:
                return None
            start, end = max(0, size - n), size - 1
        else:
            start = int(a)
            end = int(b) if b else size - 1
    except ValueError:
        return None
    if start >= size or start > end:
        return None
    return start, min(end, size - 1)


class Server(socketserver.ThreadingTCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--port', type=int, default=8080, help='TCP port (default 8080)')
    ap.add_argument('--bind', default='127.0.0.1',
                    help='address to bind (default 127.0.0.1; 0.0.0.0 for the LAN)')
    ap.add_argument('--root', default=here,
                    help='document root (default: the repo root, so both web/ '
                         'and build/web/ are reachable)')
    ap.add_argument('--quiet', action='store_true', help='no per-request log lines')
    args = ap.parse_args()

    root = os.path.abspath(args.root)
    if not os.path.isdir(root):
        sys.exit('webserve: no such directory: %s' % root)

    try:
        httpd = Server((args.bind, args.port), Handler)
    except OSError as e:
        sys.exit('webserve: cannot bind %s:%d (%s)' % (args.bind, args.port, e))
    httpd.doc_root = root
    httpd.quiet = args.quiet

    host = args.bind if args.bind not in ('0.0.0.0', '::') else socket.gethostname()
    shell = '/web/' if os.path.isdir(os.path.join(root, 'web')) else '/'
    engine = os.path.join(root, 'build', 'web', 'burnout3.js')

    print('Burnout 3 RE web shell')
    print('  serving   %s' % root)
    print('  isolation COOP: same-origin + COEP: require-corp (SharedArrayBuffer ON)')
    print('  engine    %s' % ('found: build/web/burnout3.js'
                              if os.path.isfile(engine)
                              else 'NOT BUILT yet (the page degrades gracefully)'))
    print('')
    print('    http://%s:%d%s' % (host, args.port, shell))
    print('')
    print('  Ctrl-C to stop.')
    sys.stdout.flush()      # the banner must appear even when piped into a log
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print('\nwebserve: stopped')
    finally:
        httpd.server_close()


if __name__ == '__main__':
    main()
