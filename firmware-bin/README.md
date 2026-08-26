# Online-Update (OTA)

Der ESP32 aktualisiert sich selbst: Er liest `version.json`, vergleicht die
Version mit seiner eigenen und lädt bei Bedarf `firmware.bin` und flasht sich.

## Wie es ausgeliefert wird

Die Firmware wird **tokenlos** über ein **öffentliches** Repo verteilt:
**[`teesmokr/porsche-openwb-firmware`](https://github.com/teesmokr/porsche-openwb-firmware)**
(nur `firmware.bin` + `version.json`). Der Quellcode bleibt in diesem privaten
Repo.

Die **Standard-Update-URL ist ab Werk in der Firmware eingetragen**
(`DEFAULT_UPDATE_URL`), es muss also nichts konfiguriert werden. Im
Web-Interface einfach **„Auf Updates prüfen"** → **„Update installieren"**.

Die Dateien hier im privaten Repo (`firmware-bin/`) sind eine Kopie/Archiv des
jeweils veröffentlichten Stands.

## Neue Firmware veröffentlichen

1. `FW_VERSION` in `firmware-esp32/esp32_porsche_bridge.ino` erhöhen.
2. Kompilieren und `.bin` exportieren:
   ```
   arduino-cli compile --fqbn esp32:esp32:esp32 --output-dir out firmware-esp32
   ```
3. `esp32_porsche_bridge.ino.bin` als `firmware.bin` **ins öffentliche Repo**
   kopieren, dort `version.json` auf die neue Version setzen, committen+pushen.
4. (Optional) dieselben Dateien hier unter `firmware-bin/` aktualisieren.

Der ESP32 findet das Update beim nächsten „Prüfen".

## Privates Repo als OTA-Quelle (Alternative)

Wer die `.bin` lieber privat hostet, trägt im Web-Interface eine eigene
**Update-URL** ein und dazu einen **Fine-grained Personal Access Token**
(Leserecht „Contents") als **Update-Token**. Der ESP32 sendet ihn als
`Authorization: token <...>`. Der Token liegt dann im ESP32-Flash — eng
begrenzt und widerrufbar wählen.
