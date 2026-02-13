# MiniDeviceHub8266

Kleines **Testprojekt** für den ESP8266, entstanden aus einem sehr praktischen Grund:

## Warum dieses Projekt existiert (Motivation)

Ich hatte keine Lust mehr auf ständiges **USB-Flashen** bei jedem kleinen Fix.  
Ich wollte eine **einfache OTA-Update-Funktion** bauen, die ich selbst verstehe und kontrolliere.

Wichtig: Das ist bewusst **kein Tasmota/ESPHome/Framework-Clone**.  
Ich wollte **lernen, wie OTA auf dem ESP8266 technisch funktioniert** – direkt im eigenen Code, ohne Abhängigkeit von großen Fremdprojekten.

## Was macht das Ding?

- Der ESP8266 versucht sich mit dem WLAN zu verbinden (STA).
- Wenn das nicht klappt (oder keine Daten vorhanden sind), kann er als AP starten.
- Es gibt eine Weboberfläche mit:
  - `/` Statusseite
  - `/info` JSON-Status
  - `/config` Konfiguration (optional geschützt)
  - `/update` OTA-Firmware-Upload (optional geschützt)

## Code-Erklärung (Was passiert wo?)

### 1) Includes & Konfiguration
Am Anfang stehen:
- benötigte Libraries (WiFi, WebServer, LittleFS, JSON, ggf. U8g2)
- Default-Konfigurationswerte (ohne echte Secrets)
- Konstanten: AP-Name, Ports, OLED-Setup usw.

Ziel: Alles, was das Verhalten steuert, ist zentral auffindbar.

### 2) Datenmodell: `Config`
Es gibt eine Konfigstruktur (z. B. SSID, Pass, Hostname, protect_web, web_user, web_pass …).

Die Struktur wird:
- aus `LittleFS` geladen (z. B. `config.json`)
- validiert (Felder leer? Werte plausibel?)
- wieder gespeichert (nach Änderungen in der Web-UI)

### 3) Dateisystem (LittleFS)
Beim Start:
- `LittleFS.begin()`
- Wenn `config.json` existiert → laden
- Wenn nicht → Defaults verwenden

Damit kann das Gerät nach einem Reboot seine Einstellungen behalten.

### 4) WLAN-Startlogik (STA → Fallback)
Typischer Ablauf:
- Wenn WLAN-Daten vorhanden → `WiFi.mode(WIFI_STA)` + connect
- Wenn Connect erfolgreich → weiter mit Webserver im STA-Netz
- Wenn Connect fehlschlägt → AP-Modus (z. B. `WiFi.softAP(...)`) als Fallback

Das macht das Gerät recoverable: du kommst immer wieder an die Config ran.

### 5) Webserver: Routes
Es werden Handler registriert, z. B.:
- `server.on("/", ...)` → Status HTML
- `server.on("/info", ...)` → JSON-Output
- `server.on("/config", ...)` → Config-Formular / Save
- `server.on("/update", ...)` → OTA Upload (HTTP POST)
- `server.on("/setpass", ...)` → First-Run Passwort setzen (wenn nötig)

### 6) Security: Protect-Web + First-Run Passwort
Wenn `protect_web=true`, werden sensible Bereiche geschützt.

**First-Run-Regel:**
- Wenn `protect_web=true` **und** `web_pass` leer ist,
  dann werden `/config` und `/update` automatisch auf `/setpass` umgeleitet.
- Auf `/setpass` setzt du einmalig User+Passwort.
- Danach ist die UI “normal” erreichbar (ggf. mit Login).

Damit gibt es **kein Default-Admin-Passwort** im Repo.

### 7) OTA Update (Kernfeature)
`/update` ist eine HTTP-Upload-Route:
- Browser lädt eine neue `.bin` hoch
- der ESP8266 schreibt diese Firmware über `Update` (Arduino OTA via HTTP)
- bei Erfolg: Reboot → neue Firmware ist aktiv

Das ist das Herz des Projekts: OTA ohne großen Overhead, nachvollziehbar im Code.

## Hinweis
Dieses Projekt ist ein Lern-/Testprojekt. Es ist bewusst minimal gehalten,
damit man OTA und die begleitenden Bausteine (Config, LittleFS, Webserver)
klar nachvollziehen kann.
