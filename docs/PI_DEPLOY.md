# Porsche-Modul auf die openWB (Raspberry Pi) bringen

Vorausgesetzt: Du hast dich im Windows-Tool **erfolgreich eingeloggt** und mit
„Token fuer openWB exportieren" eine Datei `token_<ID>.json` erzeugt. Die `<ID>`
muss die **Fahrzeug-Nummer** in openWB sein, der du das Modul zuweist
(erstes Fahrzeug = i. d. R. `0`).

Ersetze `<PI-IP>` durch die IP deiner openWB. Standard-Installationspfad ist
`/var/www/html/openWB` — falls abweichend, mit
`find / -maxdepth 6 -type d -name openWB 2>/dev/null` suchen und Pfade anpassen.

## 1. Dateien auf den Pi kopieren

Auf dem Windows-Rechner (im Ordner `Downloads\openWB`):

```bash
scp porsche-module.zip openwb@<PI-IP>:/tmp/
scp token_0.json      openwb@<PI-IP>:/tmp/
```

## 2. Auf dem Pi installieren

```bash
ssh openwb@<PI-IP>

# Modul entpacken
sudo unzip -o /tmp/porsche-module.zip -d /var/www/html/openWB/packages/modules/vehicles/

# Token ablegen
sudo mkdir -p /var/www/html/openWB/data/modules/porsche
sudo cp /tmp/token_0.json /var/www/html/openWB/data/modules/porsche/token_0.json

# Besitzer/Rechte an ein bestehendes Modul angleichen (robust, egal welcher User)
sudo chown --reference=/var/www/html/openWB/packages/modules/vehicles/vweuda \
  -R /var/www/html/openWB/packages/modules/vehicles/porsche
sudo chown --reference=/var/www/html/openWB/packages/modules/vehicles/vweuda \
  -R /var/www/html/openWB/data/modules/porsche
```

## 3. openWB neu starten

Am einfachsten ueber die Weboberflaeche: **System → openWB → Neustart**
(oder den ganzen Pi neu starten). Danach wird das Modul automatisch erkannt.

## 4. In der openWB konfigurieren

1. **Einstellungen → Fahrzeuge → (dein Macan) → SoC-Modul** → „Porsche Connect".
2. **E-Mail**, **Passwort** und optional **VIN** eintragen
   (VIN nur noetig, wenn mehrere Fahrzeuge im Porsche-Konto).
3. Speichern. Der erste SoC-Abruf nutzt den mitgelieferten Token — **kein
   Captcha**. Danach erneuert sich der Token selbststaendig.

## Wichtig

- **Token rotiert:** Nach dem Bootstrap das Windows-Tool **nicht mehr** fuer
  Abrufe/Export mit demselben Konto nutzen — sonst werden Pi und Tool sich
  gegenseitig abmelden. Nur neu bootstrappen, wenn der Pi den Login verliert.
- **Fahrzeug-ID muss passen:** Die Zahl in `token_<ID>.json` muss der
  openWB-Fahrzeug-Nummer entsprechen, der du das Modul zuweist. Zur Not den
  Export im Tool mit der richtigen ID wiederholen und Datei ersetzen.
- E-Mail/Passwort bleiben in der openWB-Konfiguration hinterlegt (dienen als
  Fallback), der Token verhindert nur das headless nicht loesbare Captcha.
