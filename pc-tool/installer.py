"""SSH/SFTP-Installer: kopiert das openWB-Porsche-Modul auf eine openWB und
startet den Dienst neu, sodass es im UI auswaehlbar wird."""
import json
import shlex
import socket

import paramiko


class InstallError(Exception):
    pass


DEFAULT_BASES = [
    "/var/www/html/openWB",
    "/opt/openWB",
    "/home/openwb/openWB",
]


class OpenWBInstaller:
    def __init__(self, host, username="openwb", password=None, key_path=None, port=22):
        self.host = host.strip()
        self.username = (username or "openwb").strip()
        self.password = password or None
        self.key_path = key_path or None
        self.port = int(port or 22)

    def _connect(self):
        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        kwargs = dict(hostname=self.host, port=self.port, username=self.username,
                      timeout=15, banner_timeout=15, auth_timeout=15,
                      allow_agent=False, look_for_keys=False)
        if self.key_path:
            kwargs["key_filename"] = self.key_path
            if self.password:
                kwargs["passphrase"] = self.password
        else:
            kwargs["password"] = self.password
        try:
            client.connect(**kwargs)
        except paramiko.AuthenticationException:
            raise InstallError("SSH-Anmeldung fehlgeschlagen (Benutzer/Passwort/Key pruefen).")
        except (socket.timeout, socket.error, paramiko.SSHException) as e:
            raise InstallError(f"Keine SSH-Verbindung zu {self.host}:{self.port} - {e}")
        return client

    def _run(self, client, cmd, sudo=False):
        if sudo:
            cmd = f"sudo -S -p '' bash -c {shlex.quote(cmd)}"
        stdin, stdout, stderr = client.exec_command(cmd, timeout=60)
        if sudo and self.password:
            try:
                stdin.write(self.password + "\n")
                stdin.flush()
            except OSError:
                pass
        out = stdout.read().decode("utf-8", "replace")
        err = stderr.read().decode("utf-8", "replace")
        rc = stdout.channel.recv_exit_status()
        return rc, out, err

    def _detect_base(self, client):
        for base in DEFAULT_BASES:
            rc, _, _ = self._run(client, f'test -d "{base}/packages/modules/vehicles"')
            if rc == 0:
                return base
        rc, out, _ = self._run(
            client,
            "find / -maxdepth 6 -type d -path '*/packages/modules/vehicles' 2>/dev/null | head -1")
        path = out.strip().splitlines()[0].strip() if out.strip() else ""
        if path.endswith("/packages/modules/vehicles"):
            return path[: -len("/packages/modules/vehicles")]
        raise InstallError("openWB-Installationsverzeichnis nicht gefunden.")

    def test_connection(self, log=lambda m: None):
        client = self._connect()
        try:
            log(f"Verbunden mit {self.username}@{self.host}.")
            rc, out, _ = self._run(client, "hostname; cat /etc/openwb.conf 2>/dev/null | head -1")
            log("Hostname: " + (out.strip().splitlines()[0] if out.strip() else "?"))
            base = self._detect_base(client)
            log(f"openWB gefunden: {base}")
            rc, out, err = self._run(client, "echo sudo-test", sudo=True)
            if rc == 0:
                log("sudo funktioniert.")
            else:
                log("WARNUNG: sudo-Test fehlgeschlagen - Installation braucht sudo. "
                    + (err.strip() or ""))
            return base
        finally:
            client.close()

    def install(self, files, token, vehicle_id, log=lambda m: None):
        """files: dict {relpath: content}; token: dict; vehicle_id: int."""
        client = self._connect()
        try:
            base = self._detect_base(client)
            log(f"openWB: {base}")
            vid = int(vehicle_id)

            # 1. Dateien nach /tmp hochladen (ohne sudo)
            log("Lade Moduldateien hoch ...")
            self._run(client, "rm -rf /tmp/porsche_install && mkdir -p /tmp/porsche_install/porsche")
            sftp = client.open_sftp()
            try:
                for relpath, content in files.items():
                    remote = f"/tmp/porsche_install/{relpath}"
                    parent = remote.rsplit("/", 1)[0]
                    self._run(client, f'mkdir -p "{parent}"')
                    with sftp.open(remote, "w") as fh:
                        fh.write(content)
                token_name = f"token_{vid}.json"
                with sftp.open(f"/tmp/porsche_install/{token_name}", "w") as fh:
                    fh.write(json.dumps(token))
            finally:
                sftp.close()

            # 2. An Ort und Stelle kopieren, Rechte setzen, Dienst neu starten (sudo)
            log("Installiere Modul und starte openWB neu ...")
            script = (
                "set -e; "
                f'BASE="{base}"; '
                'mkdir -p "$BASE/packages/modules/vehicles"; '
                'rm -rf "$BASE/packages/modules/vehicles/porsche"; '
                'cp -r /tmp/porsche_install/porsche "$BASE/packages/modules/vehicles/"; '
                'mkdir -p "$BASE/data/modules/porsche"; '
                f'cp /tmp/porsche_install/{token_name} "$BASE/data/modules/porsche/"; '
                'chown --reference="$BASE/packages/modules/vehicles" -R '
                '"$BASE/packages/modules/vehicles/porsche" 2>/dev/null || true; '
                'chown --reference="$BASE/packages/modules/vehicles" -R '
                '"$BASE/data/modules/porsche" 2>/dev/null || true; '
                'systemctl restart openwb2; '
                'rm -rf /tmp/porsche_install'
            )
            rc, out, err = self._run(client, script, sudo=True)
            if rc != 0:
                raise InstallError("Installation fehlgeschlagen: " + (err.strip() or out.strip()
                                   or f"Exit {rc}") + "\n(Hat der SSH-Benutzer sudo-Rechte?)")
            log("Fertig. openWB wurde neu gestartet.")
            return base
        finally:
            client.close()
