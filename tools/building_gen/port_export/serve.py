"""Serves the project root and accepts POST /save?path=<rel> (raw body) so a browser page can hand back exported GLBs.
Run from anywhere: python serve.py [port]. Only writes under assets/buildings/."""
import http.server, os, sys, urllib.parse
ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
class H(http.server.SimpleHTTPRequestHandler):
    def __init__(s, *a, **k): super().__init__(*a, directory=ROOT, **k)
    def end_headers(s): s.send_header("Access-Control-Allow-Origin", "*"); super().end_headers()
    def do_OPTIONS(s): s.send_response(204); s.send_header("Access-Control-Allow-Headers", "*"); s.end_headers()
    def do_POST(s):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(s.path).query)
        rel = q.get("path", [""])[0].replace("\\", "/")
        dst = os.path.abspath(os.path.join(ROOT, rel))
        ok = dst.startswith(os.path.join(ROOT, "assets", "buildings") + os.sep)
        if not ok: s.send_response(403); s.end_headers(); return
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = s.rfile.read(int(s.headers["Content-Length"]))
        open(dst, "wb").write(data)
        s.send_response(200); s.end_headers(); s.wfile.write(str(len(data)).encode())
http.server.ThreadingHTTPServer(("127.0.0.1", int(sys.argv[1]) if len(sys.argv) > 1 else 8765), H).serve_forever()
