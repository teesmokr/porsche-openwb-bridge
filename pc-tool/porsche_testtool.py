#!/usr/bin/env python3
"""openWB Porsche-Connect Testtool (Windows) - mit Captcha-Unterstuetzung.

Prueft, ob sich der SoC eines Porsche mit Porsche Connect auslesen laesst.
Verlangt Auth0 ein Captcha, wird das Bild im Browser geoeffnet; der Code wird
im Tool eingegeben und der Login fortgesetzt. Nach erfolgreichem Login wird ein
Token zwischengespeichert (Folge-Abrufe meist ohne Captcha).

Login-Flow/Endpunkte portiert aus pyporscheconnectapi (Apache-2.0). Inoffiziell.
Aktives Porsche-Connect-Abo noetig.
"""
import base64
import json
import os
import queue
import re
import tempfile
import threading
import time
import webbrowser
from pathlib import Path
from urllib.parse import parse_qs, urlparse

import requests
import tkinter as tk
from tkinter import ttk, messagebox, filedialog

from embedded_files import FILES as MODULE_FILES
from installer import OpenWBInstaller, InstallError
from bridge import SocBridge, lan_ip

AUTHORIZATION_SERVER = "identity.porsche.com"
AUTHORIZATION_URL = f"https://{AUTHORIZATION_SERVER}/authorize"
TOKEN_URL = f"https://{AUTHORIZATION_SERVER}/oauth/token"
REDIRECT_URI = "my-porsche-app://auth0/callback"
AUDIENCE = "https://api.porsche.com"
CLIENT_ID = "XhygisuebbrqQ80byOuU5VncxLIm8E6H"
X_CLIENT_ID = "41843fb4-691d-4970-85c7-2673e8ecef40"
API_BASE_URL = "https://api.ppa.porsche.com/app"
USER_AGENT = "openWB-porsche-testtool/1.1"
TIMEOUT = 30
SCOPE = ("openid profile email offline_access mbb ssodb badge vin dealers cars "
         "charging manageCharging plugAndCharge climatisation manageClimatisation "
         "pid:user_profile.porscheid:read pid:user_profile.name:read "
         "pid:user_profile.vehicles:read pid:user_profile.emails:read "
         "pid:user_profile.locale:read")
MEASUREMENTS = ["BATTERY_LEVEL", "E_RANGE", "RANGE", "MILEAGE", "BATTERY_CHARGING_STATE"]
TOKEN_DIR = Path.home() / ".openwb_porsche_testtool"


class PorscheApiError(Exception):
    pass


class PorscheWrongCredentials(PorscheApiError):
    pass


class PorscheCaptchaRequired(PorscheApiError):
    def __init__(self, message, image=None, state=None):
        super().__init__(message)
        self.image = image
        self.state = state


def _extract_captcha_image(html):
    """Versucht, das Captcha-Bild (data-URI oder Inline-SVG) aus der Auth0-Seite zu ziehen."""
    m = re.search(r'"image"\s*:\s*"(data:image[^"\\]+)"', html)
    if m:
        return m.group(1)
    m = re.search(r'atob\(["\']([A-Za-z0-9+/=]+)["\']\)', html)
    if m:
        try:
            ctx = json.loads(base64.b64decode(m.group(1)).decode("utf-8"))
            img = ctx.get("screen", {}).get("captcha", {}).get("image")
            if img:
                return img
        except (ValueError, json.JSONDecodeError):
            pass
    m = re.search(r'src="(data:image[^"]+)"', html)
    if m:
        return m.group(1)
    m = re.search(r'(<svg[\s\S]*?</svg>)', html)
    if m:
        return m.group(1)
    return None


class PorscheConnectApi:
    def __init__(self, email, password):
        if not email or not password:
            raise PorscheApiError("E-Mail und Passwort muessen angegeben werden.")
        self.email = email
        self.password = password
        self.session = requests.Session()
        self.session.headers.update({"User-Agent": USER_AGENT, "X-Client-ID": X_CLIENT_ID})
        self._token = self._load_token()
        self._pending_state = None

    @property
    def _token_file(self):
        return TOKEN_DIR / "token.json"

    def _load_token(self):
        try:
            with open(self._token_file, "r") as f:
                return json.load(f)
        except (FileNotFoundError, ValueError):
            return {}

    def _save_token(self):
        try:
            TOKEN_DIR.mkdir(parents=True, exist_ok=True)
            with open(self._token_file, "w") as f:
                json.dump(self._token, f)
        except OSError:
            pass

    def _token_expired(self, leeway=60):
        exp = self._token.get("expires_at")
        return True if not exp else (exp - leeway) < time.time()

    def _location_params(self, resp):
        if resp.status_code != 302 or "Location" not in resp.headers:
            raise PorscheApiError(f"Erwartete 302-Weiterleitung, erhielt {resp.status_code}.")
        return parse_qs(urlparse(resp.headers["Location"]).query)

    def _fetch_authorization_code(self):
        resp = self.session.get(AUTHORIZATION_URL, params={
            "response_type": "code", "client_id": CLIENT_ID, "redirect_uri": REDIRECT_URI,
            "audience": AUDIENCE, "scope": SCOPE, "state": "openwb",
        }, allow_redirects=False, timeout=TIMEOUT)
        params = self._location_params(resp)
        code = params.get("code", [None])[0]
        if code is not None:
            return code
        state = params.get("state", [None])[0]
        if state is None:
            raise PorscheApiError("Kein 'state' in der Auth0-Weiterleitung gefunden.")
        resume_path = self._login_with_identifier(state)
        return self._resume(resume_path)

    def _resume(self, resume_path):
        resp = self.session.get(f"https://{AUTHORIZATION_SERVER}{resume_path}",
                                allow_redirects=False, timeout=TIMEOUT)
        code = self._location_params(resp).get("code", [None])[0]
        if code is None:
            raise PorscheApiError("Kein Authorization-Code nach dem Login erhalten.")
        return code

    def _login_with_identifier(self, state, captcha_code=None):
        data = {
            "state": state, "username": self.email, "js-available": True,
            "webauthn-available": False, "is-brave": False,
            "webauthn-platform-available": False, "action": "default",
        }
        if captcha_code:
            data["captcha"] = captcha_code
        resp = self.session.post(f"https://{AUTHORIZATION_SERVER}/u/login/identifier",
                                 params={"state": state}, data=data,
                                 allow_redirects=False, timeout=TIMEOUT)
        if resp.status_code == 401:
            raise PorscheWrongCredentials("E-Mail wurde abgelehnt.")
        if resp.status_code == 400:
            # Captcha noetig (bzw. eingegebenes Captcha war falsch -> neues Bild)
            self._pending_state = state
            image = _extract_captcha_image(resp.text)
            raise PorscheCaptchaRequired(
                "Captcha erforderlich." if not captcha_code else "Captcha war falsch, bitte erneut.",
                image=image, state=state)
        # Passwort
        resp = self.session.post(f"https://{AUTHORIZATION_SERVER}/u/login/password",
                                 params={"state": state}, data={
                                     "state": state, "username": self.email,
                                     "password": self.password, "action": "default",
                                 }, allow_redirects=False, timeout=TIMEOUT)
        if resp.status_code == 400:
            raise PorscheWrongCredentials("Passwort wurde abgelehnt.")
        if "Location" not in resp.headers:
            raise PorscheApiError(f"Login-Schritt Passwort: unerwarteter Status {resp.status_code}.")
        time.sleep(2.5)
        return resp.headers["Location"]

    def submit_captcha(self, captcha_code):
        """Setzt den Login mit eingegebenem Captcha-Code fort (gleiche Session/State)."""
        if not self._pending_state:
            raise PorscheApiError("Kein offener Captcha-Vorgang.")
        resume_path = self._login_with_identifier(self._pending_state, captcha_code=captcha_code)
        code = self._resume(resume_path)
        self._exchange_code(code)
        self._pending_state = None

    def _exchange_code(self, code):
        resp = self.session.post(TOKEN_URL, data={
            "client_id": CLIENT_ID, "grant_type": "authorization_code",
            "code": code, "redirect_uri": REDIRECT_URI,
        }, timeout=TIMEOUT)
        resp.raise_for_status()
        self._store_token(resp.json())

    def _refresh(self):
        rt = self._token.get("refresh_token")
        if not rt:
            return False
        resp = self.session.post(TOKEN_URL, data={
            "client_id": CLIENT_ID, "grant_type": "refresh_token", "refresh_token": rt,
        }, timeout=TIMEOUT)
        if resp.status_code == 403:
            return False
        resp.raise_for_status()
        self._store_token(resp.json())
        return True

    def _store_token(self, data):
        data["expires_at"] = int(time.time()) + int(data.get("expires_in", 0))
        if not data.get("refresh_token") and self._token.get("refresh_token"):
            data["refresh_token"] = self._token["refresh_token"]
        self._token = data
        self._save_token()

    def _ensure_token(self):
        if not self._token_expired():
            return self._token["access_token"]
        if self._token.get("refresh_token") and self._refresh():
            return self._token["access_token"]
        self._exchange_code(self._fetch_authorization_code())
        return self._token["access_token"]

    def _api_get(self, path):
        resp = self.session.get(f"{API_BASE_URL}{path}",
                                headers={"Authorization": f"Bearer {self._ensure_token()}"},
                                timeout=TIMEOUT)
        resp.raise_for_status()
        return resp.json()

    def list_vehicles(self):
        data = self._api_get("/connect/v1/vehicles")
        return data if isinstance(data, list) else data.get("vehicles", [])

    def resolve_vin(self, configured_vin):
        if configured_vin:
            return configured_vin.strip().upper()
        vehicles = self.list_vehicles()
        if not vehicles:
            raise PorscheApiError("Keine Fahrzeuge im Porsche-Konto gefunden.")
        if len(vehicles) > 1:
            vins = ", ".join(v.get("vin", "?") for v in vehicles)
            raise PorscheApiError(f"Mehrere Fahrzeuge ({vins}). Bitte VIN angeben.")
        return vehicles[0]["vin"]

    def fetch_soc(self, configured_vin):
        vin = self.resolve_vin(configured_vin)
        query = "&".join(f"mf={m}" for m in MEASUREMENTS)
        status = self._api_get(f"/connect/v1/vehicles/{vin}?{query}")
        meas = {m["key"]: m for m in status.get("measurements", [])
                if m.get("status", {}).get("isEnabled", True)}
        battery = meas.get("BATTERY_LEVEL", {}).get("value", {})
        soc = battery.get("percent")
        if soc is None:
            raise PorscheApiError(f"Kein BATTERY_LEVEL fuer VIN {vin} erhalten.")
        rng = meas.get("E_RANGE", meas.get("RANGE", {})).get("value", {})
        range_km = rng.get("kilometers") or rng.get("value") if isinstance(rng, dict) else None
        mil = meas.get("MILEAGE", {}).get("value", {})
        odo = mil.get("kilometers") or mil.get("value") if isinstance(mil, dict) else None
        return vin, float(soc), range_km, odo


def _open_captcha_in_browser(image):
    """Schreibt das Captcha-Bild in eine temporaere HTML-Datei und oeffnet den Browser."""
    if not image:
        return False
    if image.startswith("data:"):
        body = f'<img src="{image}" style="width:320px;border:1px solid #ccc">'
    elif image.strip().startswith("<svg"):
        body = image
    else:
        body = f'<img src="data:image/svg+xml;base64,{image}" style="width:320px;border:1px solid #ccc">'
    html = ("<!doctype html><meta charset=utf-8><title>Porsche Captcha</title>"
            "<body style='font-family:sans-serif;text-align:center;padding:24px'>"
            "<h3>Porsche Login-Captcha</h3>"
            "<p>Bitte den Code ablesen und im openWB-Testtool eingeben.</p>"
            f"{body}</body>")
    path = os.path.join(tempfile.gettempdir(), "porsche_captcha.html")
    try:
        with open(path, "w", encoding="utf-8") as f:
            f.write(html)
        webbrowser.open("file:///" + path.replace("\\", "/"))
        return True
    except OSError:
        return False


class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("openWB - Porsche Connect Testtool & Installer")
        self.geometry("700x600")
        self.minsize(620, 540)
        self._queue = queue.Queue()
        self._api = None
        self._creds = None
        self._pending_fn = None
        self._captcha_image = None
        self._bridge = None

        nb = ttk.Notebook(self)
        nb.pack(fill="both", expand=True, padx=8, pady=8)
        tab_test = ttk.Frame(nb, padding=10)
        tab_bridge = ttk.Frame(nb, padding=10)
        tab_inst = ttk.Frame(nb, padding=10)
        nb.add(tab_test, text="1. SoC-Test / Login")
        nb.add(tab_bridge, text="2. SoC ohne SSH (Bridge)")
        nb.add(tab_inst, text="3. Per SSH installieren")
        self._build_test_tab(tab_test)
        self._build_bridge_tab(tab_bridge)
        self._build_install_tab(tab_inst)

        self.after(120, self._drain_queue)
        self.after(2000, self._bridge_tick)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    # ------------------------------------------------------------------ Tab 1
    def _build_test_tab(self, frm):
        ttk.Label(frm, text="Porsche Connect - SoC-Test", font=("Segoe UI", 12, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 8))

        ttk.Label(frm, text="E-Mail (Porsche ID):").grid(row=1, column=0, sticky="w", pady=3)
        self.email = ttk.Entry(frm, width=42)
        self.email.grid(row=1, column=1, columnspan=2, sticky="we", pady=3)

        ttk.Label(frm, text="Passwort:").grid(row=2, column=0, sticky="w", pady=3)
        self.password = ttk.Entry(frm, width=42, show="*")
        self.password.grid(row=2, column=1, columnspan=2, sticky="we", pady=3)

        ttk.Label(frm, text="VIN (optional):").grid(row=3, column=0, sticky="w", pady=3)
        self.vin = ttk.Entry(frm, width=42)
        self.vin.grid(row=3, column=1, columnspan=2, sticky="we", pady=3)

        self.btn_copytoken = ttk.Button(frm, text="Refresh-Token kopieren",
                                        command=self.on_copy_token)
        self.btn_copytoken.grid(row=4, column=0, sticky="we", pady=(10, 3), padx=(0, 4))
        self.btn_soc = ttk.Button(frm, text="SoC abrufen", command=self.on_soc)
        self.btn_soc.grid(row=4, column=1, sticky="we", pady=(10, 3), padx=(0, 4))
        self.btn_list = ttk.Button(frm, text="Fahrzeuge auflisten", command=self.on_list)
        self.btn_list.grid(row=4, column=2, sticky="we", pady=(10, 3), padx=(4, 0))

        self.captcha_frame = ttk.Frame(frm)
        self.captcha_frame.grid(row=5, column=0, columnspan=3, sticky="we", pady=(4, 0))
        ttk.Label(self.captcha_frame, text="Captcha-Code:").pack(side="left")
        self.captcha_entry = ttk.Entry(self.captcha_frame, width=18)
        self.captcha_entry.pack(side="left", padx=6)
        self.captcha_entry.bind("<Return>", lambda e: self.on_captcha_submit())
        ttk.Button(self.captcha_frame, text="Absenden",
                   command=self.on_captcha_submit).pack(side="left")
        ttk.Button(self.captcha_frame, text="Bild erneut oeffnen",
                   command=self._reopen_captcha).pack(side="left", padx=6)
        self.captcha_frame.grid_remove()

        self.status = ttk.Label(frm, text="Bereit.", foreground="#555")
        self.status.grid(row=6, column=0, columnspan=3, sticky="w", pady=(6, 4))

        self.out = tk.Text(frm, height=12, wrap="word", state="disabled",
                           font=("Consolas", 9), background="#f7f7f7")
        self.out.grid(row=7, column=0, columnspan=3, sticky="nsew")
        frm.rowconfigure(7, weight=1)
        frm.columnconfigure(1, weight=1)
        frm.columnconfigure(2, weight=1)

        ttk.Label(frm, text="Schritt 1: Hier einloggen (ggf. Captcha loesen), bis der SoC "
                            "erscheint. Danach Tab 2 zum Installieren.",
                  foreground="#888", font=("Segoe UI", 8), wraplength=640).grid(
            row=8, column=0, columnspan=3, sticky="w", pady=(6, 0))

    # --------------------------------------------------------- Tab 2 (Bridge)
    def _build_bridge_tab(self, frm):
        ttk.Label(frm, text="SoC-Bridge (ohne SSH, empfohlen)",
                  font=("Segoe UI", 12, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 4))
        ttk.Label(frm, text="Dieser PC holt den SoC von Porsche und stellt ihn per URL bereit. "
                            "In openWB das eingebaute SoC-Modul 'HTTP' auf diese URLs zeigen "
                            "lassen. Es wird nichts auf der openWB installiert.",
                  foreground="#666", font=("Segoe UI", 8), wraplength=650).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(0, 8))

        ttk.Label(frm, text="VIN (optional):").grid(row=2, column=0, sticky="w", pady=3)
        self.bridge_vin = ttk.Entry(frm, width=28)
        self.bridge_vin.grid(row=2, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="Port:").grid(row=3, column=0, sticky="w", pady=3)
        self.bridge_port = ttk.Entry(frm, width=8)
        self.bridge_port.insert(0, "8000")
        self.bridge_port.grid(row=3, column=1, sticky="w", pady=3)

        ttk.Label(frm, text="Aktualisierung (Min.):").grid(row=4, column=0, sticky="w", pady=3)
        self.bridge_interval = ttk.Entry(frm, width=8)
        self.bridge_interval.insert(0, "10")
        self.bridge_interval.grid(row=4, column=1, sticky="w", pady=3)

        self.btn_bridge_start = ttk.Button(frm, text="Bridge starten",
                                           command=self.on_bridge_start)
        self.btn_bridge_start.grid(row=5, column=1, sticky="we", pady=(10, 3), padx=(0, 4))
        self.btn_bridge_stop = ttk.Button(frm, text="Stoppen", command=self.on_bridge_stop,
                                          state="disabled")
        self.btn_bridge_stop.grid(row=5, column=2, sticky="we", pady=(10, 3))

        ttk.Label(frm, text="In openWB einzutragen (Fahrzeug -> SoC-Modul 'HTTP'):").grid(
            row=6, column=0, columnspan=3, sticky="w", pady=(8, 2))
        self.bridge_urls = tk.Text(frm, height=3, wrap="word", state="disabled",
                                   font=("Consolas", 9), background="#eef6ee")
        self.bridge_urls.grid(row=7, column=0, columnspan=3, sticky="we")

        self.bridge_status = ttk.Label(frm, text="Bridge gestoppt.", foreground="#555")
        self.bridge_status.grid(row=8, column=0, columnspan=3, sticky="w", pady=(6, 4))

        self.bridge_out = tk.Text(frm, height=8, wrap="word", state="disabled",
                                  font=("Consolas", 9), background="#f7f7f7")
        self.bridge_out.grid(row=9, column=0, columnspan=3, sticky="nsew")
        frm.rowconfigure(9, weight=1)
        frm.columnconfigure(1, weight=1)
        frm.columnconfigure(2, weight=1)

    def _blog(self, text, clear=False):
        self._write(self.bridge_out, text, clear)

    def _bridge_show_urls(self, ip, port):
        soc_url = f"http://{ip}:{port}/soc"
        range_url = f"http://{ip}:{port}/range"
        self._write(self.bridge_urls,
                    f"SoC-URL:   {soc_url}\nRange-URL: {range_url}", clear=True)
        self._blog(f"URLs bereit. Zum Testen im Browser: http://{ip}:{port}/status")

    def on_bridge_start(self):
        if self._api is None or not self._api._token.get("refresh_token"):
            messagebox.showwarning(
                "Kein Token", "Bitte zuerst in Tab 1 erfolgreich 'SoC abrufen' (Login).")
            return
        if self._bridge is not None:
            messagebox.showinfo("Bridge", "Bridge laeuft bereits.")
            return
        try:
            port = int(self.bridge_port.get().strip() or "8000")
            interval = float(self.bridge_interval.get().strip() or "10")
        except ValueError:
            messagebox.showerror("Fehler", "Port und Intervall muessen Zahlen sein.")
            return
        vin = self.bridge_vin.get().strip() or None
        bridge = SocBridge(self._api, vin, port=port, interval_s=int(interval * 60),
                           log=lambda m: self._queue.put(("bridge_log", m)))
        self._blog("", clear=True)
        self.bridge_status.configure(text="Starte Bridge ...")

        def worker():
            try:
                bridge.start()
                self._bridge = bridge
                self._queue.put(("bridge_started", (lan_ip(), bridge.port)))
            except OSError as e:
                self._queue.put(("bridge_error",
                                 f"Port {port} konnte nicht geoeffnet werden ({e}). "
                                 "Anderen Port versuchen oder Firewall pruefen."))
            except Exception as e:  # noqa: BLE001
                self._queue.put(("bridge_error", str(e)))
        threading.Thread(target=worker, daemon=True).start()

    def on_bridge_stop(self):
        if self._bridge:
            self._bridge.stop()
            self._bridge = None
        self.btn_bridge_start.configure(state="normal")
        self.btn_bridge_stop.configure(state="disabled")
        self.bridge_status.configure(text="Bridge gestoppt.")

    def _bridge_tick(self):
        if self._bridge:
            v = self._bridge.current()
            if v.get("error"):
                self.bridge_status.configure(
                    text=f"Letzter Fehler: {v['error'][:80]}", foreground="#a33")
            elif v.get("soc") is not None:
                import time as _t
                age = "" if not v.get("ts") else f" (vor {int((_t.time()-v['ts'])//60)} Min.)"
                self.bridge_status.configure(
                    text=f"Laeuft. SoC {v['soc']} %"
                         + ("" if v.get("range") is None else f", {v['range']} km") + age,
                    foreground="#161")
        self.after(2000, self._bridge_tick)

    def _on_close(self):
        if self._bridge:
            self._bridge.stop()
        self.destroy()

    # ------------------------------------------------------------------ Tab 3
    def _build_install_tab(self, frm):
        ttk.Label(frm, text="Modul auf openWB installieren", font=("Segoe UI", 12, "bold")).grid(
            row=0, column=0, columnspan=3, sticky="w", pady=(0, 4))
        ttk.Label(frm, text="Voraussetzung: In Tab 1 erfolgreich eingeloggt (Token vorhanden) "
                            "und SSH auf der openWB aktiv.",
                  foreground="#888", font=("Segoe UI", 8), wraplength=640).grid(
            row=1, column=0, columnspan=3, sticky="w", pady=(0, 8))

        ttk.Label(frm, text="openWB IP-Adresse:").grid(row=2, column=0, sticky="w", pady=3)
        self.host = ttk.Entry(frm, width=28)
        self.host.grid(row=2, column=1, sticky="we", pady=3)

        row = ttk.Frame(frm)
        row.grid(row=2, column=2, sticky="e")
        ttk.Label(row, text="Port:").pack(side="left")
        self.port = ttk.Entry(row, width=6)
        self.port.insert(0, "22")
        self.port.pack(side="left", padx=(4, 0))

        ttk.Label(frm, text="SSH-Benutzer:").grid(row=3, column=0, sticky="w", pady=3)
        self.ssh_user = ttk.Entry(frm, width=28)
        self.ssh_user.insert(0, "openwb")
        self.ssh_user.grid(row=3, column=1, sticky="we", pady=3)

        ttk.Label(frm, text="SSH-Passwort:").grid(row=4, column=0, sticky="w", pady=3)
        self.ssh_pass = ttk.Entry(frm, width=28, show="*")
        self.ssh_pass.grid(row=4, column=1, sticky="we", pady=3)

        ttk.Label(frm, text="SSH-Key (optional):").grid(row=5, column=0, sticky="w", pady=3)
        self.ssh_key = ttk.Entry(frm, width=28)
        self.ssh_key.grid(row=5, column=1, sticky="we", pady=3)
        ttk.Button(frm, text="...", width=3,
                   command=self._pick_key).grid(row=5, column=2, sticky="w", pady=3)

        ttk.Label(frm, text="openWB Fahrzeug-ID:").grid(row=6, column=0, sticky="w", pady=3)
        self.inst_vehicle_id = ttk.Entry(frm, width=6)
        self.inst_vehicle_id.insert(0, "0")
        self.inst_vehicle_id.grid(row=6, column=1, sticky="w", pady=3)

        self.btn_test = ttk.Button(frm, text="Verbindung testen", command=self.on_test_conn)
        self.btn_test.grid(row=7, column=1, sticky="we", pady=(10, 3), padx=(0, 4))
        self.btn_install = ttk.Button(frm, text="Jetzt installieren", command=self.on_install)
        self.btn_install.grid(row=7, column=2, sticky="we", pady=(10, 3))

        self.inst_status = ttk.Label(frm, text="Bereit.", foreground="#555")
        self.inst_status.grid(row=8, column=0, columnspan=3, sticky="w", pady=(6, 4))

        self.inst_out = tk.Text(frm, height=12, wrap="word", state="disabled",
                                font=("Consolas", 9), background="#f7f7f7")
        self.inst_out.grid(row=9, column=0, columnspan=3, sticky="nsew")
        frm.rowconfigure(9, weight=1)
        frm.columnconfigure(1, weight=1)

    def _pick_key(self):
        path = filedialog.askopenfilename(title="SSH-Private-Key waehlen")
        if path:
            self.ssh_key.delete(0, "end")
            self.ssh_key.insert(0, path)

    # ------------------------------------------------------------- gemeinsame
    def _log(self, text, clear=False):
        self._write(self.out, text, clear)

    def _ilog(self, text, clear=False):
        self._write(self.inst_out, text, clear)

    def _write(self, widget, text, clear):
        widget.configure(state="normal")
        if clear:
            widget.delete("1.0", "end")
        widget.insert("end", text + "\n")
        widget.see("end")
        widget.configure(state="disabled")

    def _set_busy(self, busy, msg=""):
        state = "disabled" if busy else "normal"
        for b in (self.btn_soc, self.btn_list, self.btn_copytoken,
                  self.btn_test, self.btn_install):
            b.configure(state=state)
        label = msg or ("Bitte warten ..." if busy else "Bereit.")
        self.status.configure(text=label)
        self.inst_status.configure(text=label)

    def _ensure_api(self):
        email, password = self.email.get().strip(), self.password.get()
        if self._api is None or self._creds != (email, password):
            self._api = PorscheConnectApi(email, password)
            self._creds = (email, password)
        return self._api

    def _dispatch(self, target):
        def worker():
            try:
                self._queue.put(("result", target()))
            except PorscheCaptchaRequired as e:
                self._queue.put(("captcha", e))
            except PorscheWrongCredentials as e:
                self._queue.put(("creds", str(e)))
            except PorscheApiError as e:
                self._queue.put(("error", str(e)))
            except requests.RequestException as e:
                self._queue.put(("error", f"Netzwerkfehler: {e}"))
            except Exception as e:  # noqa: BLE001
                self._queue.put(("error", f"Unerwarteter Fehler: {e}"))
        threading.Thread(target=worker, daemon=True).start()

    def _run(self, fn):
        try:
            self._ensure_api()
        except PorscheApiError as e:
            messagebox.showerror("Fehler", str(e))
            return
        self._pending_fn = fn
        self._set_busy(True, "Verbinde mit Porsche ...")
        self._log("", clear=True)
        self.captcha_frame.grid_remove()
        self._dispatch(fn)

    # ---------------------------------------------------------- Tab-1-Aktionen
    def on_captcha_submit(self):
        code = self.captcha_entry.get().strip()
        if not code or self._api is None or self._pending_fn is None:
            return
        self._set_busy(True, "Pruefe Captcha ...")

        def target():
            self._api.submit_captcha(code)
            return self._pending_fn()
        self._dispatch(target)

    def _reopen_captcha(self):
        if self._captcha_image and not _open_captcha_in_browser(self._captcha_image):
            self._log("Captcha-Bild konnte nicht geoeffnet werden.")

    def on_copy_token(self):
        if self._api is None or not self._api._token.get("refresh_token"):
            messagebox.showwarning(
                "Kein Token", "Bitte zuerst erfolgreich 'SoC abrufen' (Login).")
            return
        rt = self._api._token["refresh_token"]
        self.clipboard_clear()
        self.clipboard_append(rt)
        self.update()
        messagebox.showinfo(
            "Kopiert", "Refresh-Token in die Zwischenablage kopiert.\n\n"
            "Im ESP32-Web-Interface ins Feld 'Refresh-Token' einfuegen.")

    def on_soc(self):
        vin = self.vin.get().strip() or None

        def task():
            v, soc, range_km, odo = self._api.fetch_soc(vin)
            return (f"VIN:            {v}\n"
                    f"SoC:            {soc} %\n"
                    f"Reichweite:     {range_km} km\n"
                    f"Kilometerstand: {odo} km")
        self._run(task)

    def on_list(self):
        def task():
            vehicles = self._api.list_vehicles()
            lines = ["Fahrzeuge im Konto:"]
            for veh in vehicles:
                lines.append(f"  - {veh.get('vin', '?')}  {veh.get('modelName', '')} "
                             f"{veh.get('modelYear', '')}".rstrip())
            return "\n".join(lines) if vehicles else "Keine Fahrzeuge gefunden."
        self._run(task)

    # ---------------------------------------------------------- Tab-2-Aktionen
    def _make_installer(self):
        host = self.host.get().strip()
        if not host:
            messagebox.showerror("Fehler", "Bitte die IP-Adresse der openWB angeben.")
            return None
        return OpenWBInstaller(
            host=host, username=self.ssh_user.get().strip() or "openwb",
            password=self.ssh_pass.get() or None,
            key_path=self.ssh_key.get().strip() or None,
            port=self.port.get().strip() or "22")

    def on_test_conn(self):
        inst = self._make_installer()
        if inst is None:
            return
        self._set_busy(True, "Teste SSH-Verbindung ...")
        self._ilog("", clear=True)

        def worker():
            try:
                base = inst.test_connection(log=lambda m: self._queue.put(("inst_log", m)))
                self._queue.put(("inst_result", f"Verbindung OK. openWB unter: {base}"))
            except InstallError as e:
                self._queue.put(("inst_error", str(e)))
            except Exception as e:  # noqa: BLE001
                self._queue.put(("inst_error", f"Unerwarteter Fehler: {e}"))
        threading.Thread(target=worker, daemon=True).start()

    def on_install(self):
        if self._api is None or not self._api._token.get("refresh_token"):
            messagebox.showwarning(
                "Kein Token", "Bitte zuerst in Tab 1 erfolgreich 'SoC abrufen', damit ein "
                "gueltiger Token vorliegt, der mitinstalliert wird.")
            return
        inst = self._make_installer()
        if inst is None:
            return
        try:
            vid = int(self.inst_vehicle_id.get().strip() or "0")
        except ValueError:
            messagebox.showerror("Fehler", "Fahrzeug-ID muss eine Zahl sein (z. B. 0).")
            return
        if not messagebox.askyesno(
                "Installieren",
                f"Modul + Token (Fahrzeug-ID {vid}) auf {inst.host} installieren und openWB "
                "neu starten?\n\nHinweis: Danach das Tool nicht mehr fuer Abrufe nutzen "
                "(Token rotiert)."):
            return
        token = dict(self._api._token)
        self._set_busy(True, "Installiere ...")
        self._ilog("", clear=True)

        def worker():
            try:
                base = inst.install(MODULE_FILES, token, vid,
                                    log=lambda m: self._queue.put(("inst_log", m)))
                self._queue.put(("inst_result",
                                 f"Installation abgeschlossen ({base}).\n\nJetzt in der openWB: "
                                 "Einstellungen -> Fahrzeuge -> SoC-Modul 'Porsche Connect' "
                                 "waehlen, E-Mail/Passwort/VIN eintragen, speichern."))
            except InstallError as e:
                self._queue.put(("inst_error", str(e)))
            except Exception as e:  # noqa: BLE001
                self._queue.put(("inst_error", f"Unerwarteter Fehler: {e}"))
        threading.Thread(target=worker, daemon=True).start()

    # ------------------------------------------------------------- Queue-Pump
    def _drain_queue(self):
        try:
            while True:
                kind, payload = self._queue.get_nowait()
                if kind == "inst_log":
                    self._ilog(payload)
                    continue
                if kind == "bridge_log":
                    self._blog(payload)
                    continue
                if kind == "bridge_started":
                    ip, port = payload
                    self._bridge_show_urls(ip, port)
                    self.btn_bridge_start.configure(state="disabled")
                    self.btn_bridge_stop.configure(state="normal")
                    self.bridge_status.configure(text="Bridge laeuft.", foreground="#161")
                    continue
                if kind == "bridge_error":
                    self.bridge_status.configure(text="Fehler.", foreground="#a33")
                    self._blog(payload)
                    self.btn_bridge_start.configure(state="normal")
                    messagebox.showerror("Bridge-Fehler", payload)
                    continue
                self._set_busy(False)
                if kind == "result":
                    self.captcha_frame.grid_remove()
                    self.status.configure(text="Fertig.")
                    self._log(payload)
                elif kind == "captcha":
                    self._captcha_image = payload.image
                    self.captcha_frame.grid()
                    self.captcha_entry.delete(0, "end")
                    self.captcha_entry.focus_set()
                    opened = _open_captcha_in_browser(payload.image)
                    if opened:
                        self.status.configure(text="Captcha im Browser ablesen und hier eingeben.")
                    elif payload.image:
                        self.status.configure(text="Captcha - siehe Ausgabe.")
                        self._log("Captcha-Bild (im Browser oeffnen):")
                        self._log(payload.image)
                    else:
                        self.status.configure(text="Captcha erforderlich, kein Bild lesbar.")
                        messagebox.showwarning(
                            "Captcha", "Porsche verlangt ein Captcha, das Bild konnte aber nicht "
                            "ausgelesen werden. Bitte spaeter erneut versuchen.")
                elif kind == "creds":
                    self.captcha_frame.grid_remove()
                    self.status.configure(text="Zugangsdaten falsch.")
                    messagebox.showerror("Zugangsdaten falsch", payload)
                elif kind == "error":
                    self.status.configure(text="Fehler.")
                    messagebox.showerror("Fehler", payload)
                    self._log(payload)
                elif kind == "inst_result":
                    self.inst_status.configure(text="Fertig.")
                    self._ilog(payload)
                    messagebox.showinfo("openWB", payload)
                elif kind == "inst_error":
                    self.inst_status.configure(text="Fehler.")
                    self._ilog(payload)
                    messagebox.showerror("Installations-Fehler", payload)
        except queue.Empty:
            pass
        self.after(120, self._drain_queue)


if __name__ == "__main__":
    App().mainloop()
