# The test server: a file, an echo of a POST's hash, a 404 and a request
# that is never answered.
import hashlib, http.server, os, sys, time
port, root = int(sys.argv[1]), sys.argv[2]
class H(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def log_message(self, *a): pass
    def reply(self, code, body, ctype="application/octet-stream"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def do_GET(self):
        if self.path == "/body.bin":
            with open(os.path.join(root, "body.bin"), "rb") as f:
                self.reply(200, f.read())
        elif self.path == "/stall":
            time.sleep(30)
        else:
            self.reply(404, b"not here", "text/plain")
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        data = self.rfile.read(n)
        out = "%s %s %s" % (hashlib.sha256(data).hexdigest(), self.headers.get("X-Test"), self.headers.get("Content-Type"))
        self.reply(200, out.encode(), "text/plain")
class S(http.server.ThreadingHTTPServer):
    allow_reuse_address = True
    daemon_threads = True
S(("127.0.0.1", port), H).serve_forever()
