# WireGuard-Tunnel: Deutschland ⇄ Kosovo (ESP32 / ESP32-C3)

```
Deine Geräte ──WLAN──▶ ESP32 DevKit (DE) ══WireGuard══▶ ESP32-C3 (XK) ──WLAN──▶ Router Kosovo ──▶ Internet
                       SoftAP "KosovoVPN"   UDP 51820     NAPT
```

Alles, was an das SoftAP-WLAN des DevKits in Deutschland angeschlossen ist, verlässt das Internet
über den Router im Kosovo (Ziel-Server sehen die dortige öffentliche IP).

**Status: nicht auf echter Hardware getestet und nicht kompiliert** (hier gab es kein ESP-IDF).
Gegen die Quellen der Bibliothek geprüft, aber rechne mit Feinschliff beim ersten Flashen.

## Voraussetzungen (alles kostenlos)
- ESP-IDF 5.x (Bibliothek `trombik/esp_wireguard` wird beim Build automatisch geladen, BSD-3)
- Dynamisches DNS für den deutschen Router, z. B. DuckDNS
- Im **deutschen** Router: Portweiterleitung **UDP 51820 → IP des DevKits**
- Im Kosovo-Router ist nichts nötig (der C3 baut den Tunnel nach außen auf)

## Schlüssel erzeugen
```
python3 tools/wgkeys.py      # einmal für das DevKit
python3 tools/wgkeys.py      # einmal für den C3
```
Jedes Gerät bekommt seinen **privaten** Schlüssel und den **öffentlichen** des anderen. Optional `--psk`.

## Bauen und flashen
```
cd germany_gateway_esp32
idf.py set-target esp32
idf.py menuconfig        # Menü "Deutschland-Gateway": WLANs, Schlüssel, Ports
idf.py flash monitor

cd ../kosovo_exit_c3
idf.py set-target esp32c3
idf.py menuconfig        # Menü "Kosovo Exit-Node": WLAN, Schlüssel, DDNS-Name
idf.py flash monitor
```
Der C3 kann zu Hause eingestellt und geflasht werden, im Kosovo braucht er nur Strom und WLAN-Zugang.
Zugangsdaten stehen in `sdkconfig`, diese Datei nicht weitergeben oder committen.

## Test
1. Beide Logs zeigen `Tunnel STEHT`.
2. Mit dem Handy ins WLAN „KosovoVPN“, dann eine „Meine IP“-Seite öffnen: es sollte die IP aus dem Kosovo erscheinen.

## Wichtige Eigenschaften und Grenzen
- **Kill-Switch:** Fällt der Tunnel aus, haben die Clients kein Internet (kein Leck über die deutsche IP).
- **Durchsatz:** realistisch ca. 1–3 Mbit/s (WireGuard-Verschlüsselung in Software, WLAN-Radio des
  DevKits teilt sich STA und SoftAP, C3 ist langsamer). Gut für Surfen, Messenger, SSH, nicht für Streaming in HD.
- **MTU:** Der Tunnel hat eine kleinere MTU als 1500. Falls einzelne Seiten hängen, am Client die MTU auf
  ca. 1380 setzen.
- **Zeit:** WireGuard braucht synchrone Uhren, beide Geräte holen die Zeit per NTP.
- **Selbstheilung:** Steht der Tunnel am C3 länger als 3 Minuten nicht, startet er neu (neue DNS-Auflösung, neuer Handshake).
- **Nur IPv4**, ein Tunnel, ein Peer (Einschränkung der Bibliothek, Status "alpha").
- **SoftAP-Passwort** unbedingt setzen, sonst kann jeder in Reichweite deinen Kosovo-Anschluss nutzen.
- Nutze das nur mit eigenen Anschlüssen und im Rahmen der Router-/Provider-Bedingungen.
