# ESP32 Porsche-Connect SoC-Bridge für openWB

Kleine, dedizierte Bridge auf einem ESP32: holt den SoC deines Porsche über
Porsche Connect und stellt ihn per HTTP bereit. openWB liest ihn mit dem
eingebauten SoC-Modul **HTTP**. Läuft dauerhaft — kein PC nötig.

Getestet: kompiliert mit ESP32-Arduino-Core 2.0.17, ArduinoJson 7.4.3
(72 % Flash, 14 % RAM — passt auf jeden Standard-ESP32).

## Hardware

- Ein beliebiges **ESP32-Dev-Board** (z. B. ESP32-WROOM DevKitC).
- Stromversorgung über USB oder 5 V/GND (z. B. aus der openWB).
- WLAN-Empfang am Einbauort sicherstellen.

## Anmeldung — zwei Wege

**Weg A (empfohlen, ohne PC): direkt im Web-Interface.** Der ESP32 kann den
Porsche-Login inklusive **Captcha** selbst durchführen: E-Mail/Passwort im
Web-Interface eingeben → falls ein Captcha kommt, wird das Bild angezeigt →
Code eintippen → fertig. Der Refresh-Token wird im Flash gespeichert.

> Experimentell: Der komplette Auth0-Login (Cookies/Redirects) läuft auf dem
> ESP32. Klappt es nicht (Porsche ändert gelegentlich den Ablauf), nutze Weg B.

**Weg B (Fallback): Refresh-Token am PC holen.**
1. `openWB-Porsche-Tool.exe` am PC, **Tab 1**: einloggen (ggf. Captcha).
2. **„Refresh-Token kopieren"** → im Web-Interface des ESP32 ins Feld
   „Refresh-Token" einfügen.

## Online-Update (OTA)

Der ESP32 kann sich über dein Git-Repo selbst aktualisieren: im Web-Interface
unter „Einrichtung" die **Update-URL** (Roh-URL der `version.json`) eintragen,
dann **„Auf Updates prüfen"** → **„Update installieren"**. Details und Setup
für private Repos: siehe `firmware-bin/README.md` im Repository.

## Firmware flashen

**Mit Arduino IDE:**
1. In *Datei → Voreinstellungen → Zusätzliche Boardverwalter-URLs* eintragen:
   `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
2. *Werkzeuge → Board → Boardverwalter* → **esp32** (Espressif) installieren.
3. *Werkzeuge → Bibliotheken verwalten* → **ArduinoJson** (v7) installieren.
4. `esp32_porsche_bridge.ino` öffnen, Board z. B. „ESP32 Dev Module" wählen,
   ESP32 per USB anstecken, richtigen COM-Port wählen, **Hochladen**.

**Mit arduino-cli:**
```
arduino-cli core install esp32:esp32
arduino-cli lib install ArduinoJson
arduino-cli compile --fqbn esp32:esp32:esp32 esp32_porsche_bridge
arduino-cli upload -p COM3 --fqbn esp32:esp32:esp32 esp32_porsche_bridge
```

## Erste Einrichtung (Web-Interface)

1. Nach dem ersten Start öffnet der ESP32 automatisch einen **Setup-Hotspot**
   **`openWB-Porsche-Bridge`** (Passwort `porsche1234`).
2. Mit dem Hotspot verbinden — dank **Captive Portal** öffnet sich die
   Konfigurationsseite **automatisch** (wie bei Hotel-WLAN). Falls nicht,
   im Browser **http://192.168.4.1** öffnen. Die Seite springt direkt in die
   WLAN-Einrichtung.
3. Eintragen: **WLAN-Name + Passwort**, **Refresh-Token** (eingefügt),
   optional **VIN**, **Intervall** (Minuten). **Speichern** → ESP32 startet neu
   und verbindet sich mit deinem WLAN.
4. Die neue IP steht danach im Web-Interface (oder im seriellen Monitor /
   deinem Router). Öffne **http://\<ESP-IP\>/** — dort siehst du Status und die
   fertigen openWB-URLs.

## In openWB eintragen

Einstellungen → Fahrzeuge → (dein Macan) → **SoC-Modul „HTTP"**:
- **SoC-URL:** `http://<ESP-IP>/soc`
- **Range-URL:** `http://<ESP-IP>/range`

Speichern — fertig. Zum Prüfen im Browser: `http://<ESP-IP>/status`.

## Hinweise

- **Feste IP empfohlen:** Im Router dem ESP32 eine feste IP (DHCP-Reservierung)
  geben, damit die openWB-URLs stabil bleiben.
- **Token rotiert:** Der ESP32 speichert den erneuerten Refresh-Token selbst im
  Flash. Nutze denselben Token **nicht** parallel im PC-Tool, sonst melden sich
  beide gegenseitig ab. Schlägt der Refresh dauerhaft fehl (Status zeigt
  „Refresh-Token ungültig"), am PC neu einloggen und den neuen Token eintragen.
- **TLS:** Die HTTPS-Verbindungen laufen ohne Zertifikatsprüfung
  (`setInsecure()`) — verschlüsselt, aber nicht authentifiziert. Für ein
  LAN-Gerät üblich; wer es strenger mag, kann die Porsche-Root-CAs einpflegen.
- **Inoffiziell:** Porsche-Connect-Schnittstelle ist nicht offiziell; sie kann
  sich ändern. Aktives Porsche-Connect-Abo nötig.
