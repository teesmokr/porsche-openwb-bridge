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

## Voraussetzung: Refresh-Token (einmalig am PC holen)

Der ESP32 kann sich **nicht** komplett neu einloggen (Porsche verlangt ein
Captcha). Er braucht einen **Refresh-Token**, den du einmalig erzeugst:

1. `openWB-Porsche-Tool.exe` am PC starten, **Tab 1**: E-Mail/Passwort →
   „SoC abrufen" (ggf. Captcha lösen), bis der SoC erscheint.
2. Button **„Refresh-Token kopieren"** (Tab 1) — der Token liegt dann in der
   Zwischenablage. (Alternativ „Token exportieren" und den Wert
   `refresh_token` aus der JSON-Datei kopieren.)

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

1. Nach dem ersten Start öffnet der ESP32 ein WLAN
   **`openWB-Porsche-Bridge`** (Passwort `porsche1234`).
2. Damit verbinden, im Browser **http://192.168.4.1** öffnen.
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
