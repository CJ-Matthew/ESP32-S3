#!/usr/bin/env python3
"""Local dev proxy for the LED matrix simulator.

Serves the simulator's static files AND proxies /connected to the Wi-Fi presence
backend, injecting the API key server-side. This lets the browser sim read live
presence data with no CORS and without embedding the key in the page — the proxy
talks to the backend server-to-server (not a browser, so CORS never applies), and
the browser only ever makes a same-origin request to this proxy.

Usage:
    WIFI_API_KEY=<key> python3 dev_proxy.py
    # then open http://localhost:8080/

The key stays in your shell/env — never commit it. The public endpoint's CORS
policy is left untouched; only your machine (with the existing key) hits it.
"""
import os
import urllib.request
from functools import partial
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

BACKEND = os.getenv("WIFI_BACKEND", "https://web-production-316b.up.railway.app")
API_KEY = os.getenv("WIFI_API_KEY", "")
PORT = int(os.getenv("PORT", "8080"))


PROXIED = ("/connected", "/weather")  # backend GET routes forwarded with the key


class Handler(SimpleHTTPRequestHandler):
    def do_GET(self):
        path = self.path.split("?")[0].rstrip("/") or "/"
        if path in PROXIED:
            return self._proxy(path)
        return super().do_GET()

    def _proxy(self, path):
        if not API_KEY:
            self.send_error(500, "WIFI_API_KEY not set")
            return
        req = urllib.request.Request(f"{BACKEND}{path}", headers={"X-API-Key": API_KEY})
        try:
            with urllib.request.urlopen(req, timeout=10) as upstream:
                body = upstream.read()
        except Exception as exc:  # noqa: BLE001 — dev tool, surface anything
            self.send_error(502, f"proxy error: {exc}")
            return
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


def main():
    directory = os.path.dirname(os.path.abspath(__file__))
    handler = partial(Handler, directory=directory)
    print(f"Simulator + presence proxy on http://localhost:{PORT}/  (backend: {BACKEND})")
    if not API_KEY:
        print("  ⚠  WIFI_API_KEY not set — /connected will 500."
              " Run:  WIFI_API_KEY=<key> python3 dev_proxy.py")
    ThreadingHTTPServer(("127.0.0.1", PORT), handler).serve_forever()


if __name__ == "__main__":
    main()
