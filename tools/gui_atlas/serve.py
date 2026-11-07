# Serves the repository root on localhost (render.html needs Assets/, industry_models_data.js and roadlib.js) and saves
# what the pages post:  POST /save?path=<path under tools/gui_atlas or assets_src/gui>  (body = file bytes).
#   python tools/gui_atlas/serve.py [port]
#   http://localhost:<port>/tools/gui_atlas/render.html   every build item's preview -> tools/gui_atlas/pv/items/
#   http://localhost:<port>/tools/gui_atlas/atlas.html    icons + previews -> assets_src/gui/glass_atlas.png / .txt
import http.server, os, sys, urllib.parse

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, '..', '..'))
ALLOWED = [HERE, os.path.join(ROOT, 'assets_src', 'gui')]


class H(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *a, **k):
        super().__init__(*a, directory=ROOT, **k)

    def log_message(self, *a):
        pass

    def do_POST(self):
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        rel = q.get('path', [''])[0]
        dest = os.path.normpath(os.path.join(ROOT, rel))
        if not rel or not any(dest.startswith(a + os.sep) for a in ALLOWED):
            self.send_error(403)
            return
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, 'wb') as f:
            f.write(self.rfile.read(int(self.headers['Content-Length'])))
        self.send_response(200)
        self.end_headers()


http.server.ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1]) if len(sys.argv) > 1 else 8821), H).serve_forever()
