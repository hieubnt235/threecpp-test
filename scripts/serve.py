#!/usr/bin/env python3
"""Serve the built threepp spike (dist/) on http://localhost:8016/.

Plain HTTP is fine: the spike is built without -pthread, so no
SharedArrayBuffer / COOP-COEP headers are needed, and WebGL is allowed on
localhost. Run `pixi run build` first.
"""

import functools
import http.server
import os
import socketserver

PORT = 8016
HERE = os.path.dirname(os.path.abspath(__file__))
DIST = os.path.join(os.path.dirname(HERE), "dist")


class QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args):  # silence per-request noise
        pass


def main() -> None:
    if not os.path.isfile(os.path.join(DIST, "index.html")):
        raise SystemExit("dist/ is empty -- run `pixi run build` first.")
    handler = functools.partial(QuietHandler, directory=DIST)
    socketserver.TCPServer.allow_reuse_address = True
    with socketserver.TCPServer(("127.0.0.1", PORT), handler) as httpd:
        print(f"threepp spike -> http://localhost:{PORT}/   (Ctrl+C to stop)")
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nstopped.")


if __name__ == "__main__":
    main()
