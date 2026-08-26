# Online-Update (OTA) über dieses Repo

Der ESP32 kann sich selbst aktualisieren: Er liest `version.json`, vergleicht die
Version mit seiner eigenen und lädt bei Bedarf `firmware.bin` und flasht sich.

## Dateien

- `firmware.bin` — kompilierte Firmware (App-Partition), die der ESP32 lädt.
- `version.json` — `{ "version": "...", "bin": "<URL zur firmware.bin>" }`.

## Einrichtung im ESP32

Im Web-Interface unter **Einrichtung**:
- **Update-URL** = Roh-URL deiner `version.json`, z. B.
  `https://raw.githubusercontent.com/teesmokr/porsche-openwb-bridge/main/firmware-bin/version.json`
- **Update-Token** = nur nötig, wenn das Repo **privat** ist (siehe unten).

Dann im Web-Interface: **„Auf Updates prüfen"** → bei neuer Version
**„Update installieren"**. Der ESP32 flasht und startet neu.

## Öffentliches vs. privates Repo

- **Öffentliches Repo:** Kein Token nötig. Die Roh-URLs sind frei abrufbar.
- **Privates Repo:** `raw.githubusercontent.com` verlangt Authentifizierung.
  Lege einen **Fine-grained Personal Access Token** an (nur Leserecht
  „Contents" für dieses Repo) und trage ihn als **Update-Token** ein. Der
  ESP32 sendet ihn als `Authorization: token <...>`.
  > Achtung: Der Token liegt dann im ESP32-Flash. Nutze einen eng begrenzten,
  > widerrufbaren Token.

## Neue Firmware veröffentlichen

1. Version in `firmware-esp32/esp32_porsche_bridge.ino` erhöhen
   (`FW_VERSION`).
2. Neu kompilieren und die `.bin` exportieren:
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32 \
     --output-dir out firmware-esp32
   cp out/esp32_porsche_bridge.ino.bin firmware-bin/firmware.bin
   ```
3. `firmware-bin/version.json` auf die neue Versionsnummer setzen.
4. Committen und pushen. Der ESP32 findet das Update beim nächsten „Prüfen".

> Die in `bin` hinterlegte URL muss auf die **firmware.bin** in deinem Repo
> zeigen (Platzhalter `teesmokr` ersetzen).
