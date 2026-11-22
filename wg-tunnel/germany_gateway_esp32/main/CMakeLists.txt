# Alle .c-Dateien in diesem Ordner werden automatisch mitkompiliert.
# WHOLE_ARCHIVE sorgt dafuer, dass sich auch Module ohne direkte Referenz
# (mod_*.c) beim Start selbst anmelden koennen.
# Neue Datei hinzugefuegt?  ->  einmal "idf.py reconfigure" ausfuehren.
idf_component_register(SRC_DIRS "."
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash lwip driver
                       WHOLE_ARCHIVE)
