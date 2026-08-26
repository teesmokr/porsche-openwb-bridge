# PC-Tool bauen (Windows .exe)

Das Tool ist reines Python/Tkinter. Zum Ausführen aus dem Quellcode:

```bash
pip install requests paramiko
python porsche_testtool.py
```

## Als eigenständige .exe verpacken

```bash
pip install pyinstaller requests paramiko
pyinstaller --onefile --windowed --name "openWB-Porsche-Tool" \
  --collect-all paramiko --collect-submodules cryptography \
  porsche_testtool.py
```

Die fertige `openWB-Porsche-Tool.exe` liegt danach in `dist/`.

## Dateien

- `porsche_testtool.py` — GUI mit 3 Tabs (SoC-Test/Login, SoC-Bridge ohne SSH,
  SSH-Installer).
- `bridge.py` — lokaler HTTP-Dienst, der den SoC für openWB bereitstellt.
- `installer.py` — SSH/SFTP-Installer für das native openWB-Modul.
- `embedded_files.py` — eingebettete openWB-Moduldateien (vom Installer genutzt).
  Wird aus `openwb-module/porsche/` erzeugt; bei Änderungen am Modul neu
  generieren.

> Hinweis: `embedded_files.py` ist eine eingebettete Kopie von
> `../openwb-module/porsche/`. Wenn du das Modul änderst, diese Datei neu
> erzeugen (Dict `FILES` mit den Dateiinhalten).
