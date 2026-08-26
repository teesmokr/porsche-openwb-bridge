# Add Porsche Connect SoC module for vehicles

## Was

Neues Fahrzeug-SoC-Modul `porsche`, das Ladestand (SoC), Reichweite und
Kilometerstand über **Porsche Connect** ausliest (Macan EV ab 2024, Taycan,
Cayenne E3, Panamera G2, 911 ab 992, 718). Voraussetzung: aktives
Porsche-Connect-Abo + My-Porsche-Zugangsdaten.

Das Modul wird wie die übrigen Fahrzeug-Module automatisch erkannt; die
Einstellungen (E-Mail, Passwort, optionale VIN) sind über das generische
JSON-Formular editierbar – analog zu `tronity`, `vweuda`, `http`.

## Warum

Porsche wird bislang von keinem SoC-Modul unterstützt. Der EU-Data-Act-Weg
(wie bei `vweuda`) scheidet aus: Porsche ist nicht am VW-Konzernportal
angebunden, und das eigene Porsche-Portal liefert nur manuell/mit ~24 h
Verzögerung – für die Ladesteuerung unbrauchbar. Bleibt Porsche Connect.

## Umsetzung

- `api.py`: schlanker synchroner Client auf Basis von `requests` (kein neuer
  Dependency). Auth0-„Identifier First"-Login inkl. Captcha-Behandlung,
  Token-Caching/-Refresh, schonender „stored overview"-Abruf (weckt das Auto
  nicht). Login-Flow und Endpunkte portiert aus der Community-Bibliothek
  [pyporscheconnectapi](https://github.com/CJNE/pyporscheconnectapi)
  (Apache-2.0; mit GPLv3 kompatibel, Attribution im Code/README erhalten).
- `soc.py`: Anbindung an `ConfigurableVehicle` → `CarState`.
- `config.py`: `PorscheConnect(Configuration)`, `official=False`.
- `cli.py`: kleines Standalone-Tool zum Live-Test (optional, kann entfernt
  werden, falls unerwünscht).
- `porsche_test.py`: 8 Unit-Tests (gemocktes HTTP) für Parsing,
  VIN-Auflösung, Token-Refresh und Direct-Charging-Kommando.

## Getestet

- 8/8 Unit-Tests grün, flake8 sauber (max-line-length 120).
- **Live gegen einen Porsche Macan (MY2026) verifiziert**: Login (inkl.
  Captcha), SoC/Reichweite/Kilometerstand korrekt ausgelesen; Feldnamen
  `BATTERY_LEVEL.percent`, `E_RANGE.kilometers`, `MILEAGE.kilometers`
  bestätigt.

## Hinweise

- Inoffizielle Schnittstelle (`official=False`) – kann sich seitens Porsche
  ändern.
- Optionaler Direct-Charging-Befehl ist bewusst **nicht** in die automatische
  Regelung eingebunden (openWB regelt über die Wallbox); reines Komfort-Extra.
