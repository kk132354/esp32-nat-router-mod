# WireGuard-Tunnel Deutschland <-> Kosovo (ESP32 + ESP32-C3)

Geraete in Deutschland verbinden sich mit dem WLAN "KosovoVPN" am ESP32 DevKit. Ihr Internetverkehr
laeuft per WireGuard zum ESP32-C3 im Kosovo und geht dort ueber den lokalen Router ins Internet.

Ordner
- germany_gateway_esp32/  Firmware fuer das DevKit in Deutschland (Ziel esp32)
- kosovo_exit_c3/         Firmware fuer den C3 im Kosovo (Ziel esp32c3)
- tools/wgkeys.py         Schluessel erzeugen (nur Python)
- MODULE_VORLAGE.c.txt    Vorlage fuer neue Module

Bauen (je Ordner): rm -f sdkconfig; idf.py set-target <esp32|esp32c3>; idf.py build; idf.py merge-bin
Ergebnis: build/merged-binary.bin, flashen bei Adresse 0.

Module: Jede Datei main/mod_<name>.c wird automatisch eingebunden (siehe MODULE_VORLAGE.c.txt).
Nach dem Hinzufuegen einer Datei einmal "idf.py reconfigure".

Einstellungen (WLAN, Schluessel, DuckDNS-Name) stehen in sdkconfig.defaults, NICHT ins oeffentliche Git!
Wichtig in beiden sdkconfig.defaults: CONFIG_LWIP_PPP_SUPPORT=y (Workaround fuer esp_wireguard + ESP-IDF 5.x).

Status: nicht auf allen Geraeten ausfuehrlich getestet. Durchsatz nur etwa 1-3 Mbit/s.
