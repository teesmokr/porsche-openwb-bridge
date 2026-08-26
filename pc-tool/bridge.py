"""SoC-Bridge ohne SSH: holt den SoC von Porsche und stellt ihn lokal per HTTP
bereit. openWB fragt die URL mit seinem eingebauten 'HTTP'-SoC-Modul ab.

Endpunkte:
  GET /soc     -> Ladestand als ganze Zahl (z. B. 95)   [fuer soc_url]
  GET /range   -> Reichweite in km als Zahl (z. B. 326)  [fuer range_url]
  GET /status  -> JSON mit allen Werten (zum Pruefen im Browser)
"""
import json
import socket
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def lan_ip():
    """Ermittelt die vermutliche LAN-IP dieses Rechners (ohne echten Verbindungsaufbau)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.168.255.255", 1))
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


class SocBridge:
    def __init__(self, api, vin, port=8000, interval_s=600, log=lambda m: None):
        self.api = api
        self.vin = vin or None
        self.port = int(port)
        self.interval_s = max(60, int(interval_s))
        self.log = log
        self._value = {"soc": None, "range": None, "ts": None, "error": None}
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._httpd = None
        self._threads = []

    def _refresh_once(self):
        try:
            _vin, soc, range_km, _odo = self.api.fetch_soc(self.vin)
            with self._lock:
                self._value = {"soc": int(round(soc)),
                               "range": None if range_km is None else float(range_km),
                               "ts": int(time.time()), "error": None}
            self.log(f"SoC aktualisiert: {int(round(soc))} %"
                     + ("" if range_km is None else f", {range_km} km"))
        except Exception as e:  # noqa: BLE001
            with self._lock:
                self._value["error"] = str(e)
            self.log(f"Fehler beim Abruf: {e}")

    def _refresher(self):
        while not self._stop.wait(self.interval_s):
            self._refresh_once()

    def _make_handler(self):
        bridge = self

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass  # kein stdout-Spam

            def _send(self, code, body, ctype="text/plain"):
                data = body.encode("utf-8")
                self.send_response(code)
                self.send_header("Content-Type", ctype)
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)

            def do_GET(self):
                path = self.path.split("?", 1)[0].rstrip("/")
                with bridge._lock:
                    val = dict(bridge._value)
                if path == "/soc":
                    if val["soc"] is None:
                        self._send(503, "no data")
                    else:
                        self._send(200, str(val["soc"]))
                elif path == "/range":
                    if val["range"] is None:
                        self._send(503, "no data")
                    else:
                        self._send(200, str(val["range"]))
                elif path in ("", "/status"):
                    self._send(200, json.dumps(val), "application/json")
                else:
                    self._send(404, "not found")

        return Handler

    def start(self):
        # Erst einmal sofort abrufen, damit gleich ein Wert bereitsteht.
        self._refresh_once()
        self._httpd = ThreadingHTTPServer(("0.0.0.0", self.port), self._make_handler())
        t_srv = threading.Thread(target=self._httpd.serve_forever, daemon=True)
        t_ref = threading.Thread(target=self._refresher, daemon=True)
        t_srv.start()
        t_ref.start()
        self._threads = [t_srv, t_ref]
        self.log(f"Bridge laeuft auf Port {self.port}.")

    def stop(self):
        self._stop.set()
        if self._httpd:
            try:
                self._httpd.shutdown()
                self._httpd.server_close()
            except OSError:
                pass
        self._httpd = None
        self.log("Bridge gestoppt.")

    def current(self):
        with self._lock:
            return dict(self._value)
