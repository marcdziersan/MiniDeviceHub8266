/*
  MiniDeviceHub8266 - NodeMCU v2 + OLED via U8g2 (SSD1306 I2C 128x64, addr 0x3C)
  I2C Pins:
    SDA = GPIO12
    SCL = GPIO14

  OLED zeigt:
    - "Verbindung zum Router ..." (während Connect)
    - "Verbunden" + IP
    - "Keine Verbindung... AP aktiv" + AP-IP

  Libs:
    - ESP8266 core
    - ArduinoJson
    - U8g2
*/

#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <Updater.h>

#include <Wire.h>
#include <U8g2lib.h>

// ===== OLED Settings =====
// I2C pins (GPIO numbers)
#define OLED_SDA_PIN 12   // GPIO12
#define OLED_SCL_PIN 14   // GPIO14
#define OLED_ADDR    0x3C

// SSD1306 128x64, I2C
// U8g2 nutzt intern Wire; Pins setzen wir via Wire.begin(SDA,SCL).
U8G2_SSD1306_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ U8X8_PIN_NONE);

// -------- Versioning --------
static const char* FW_NAME    = "MiniDeviceHub8266";
static const char* FW_VERSION = "0.1.0";
static const char* FW_BUILD   = __DATE__ " " __TIME__;

// -------- Web server --------
ESP8266WebServer server(80);

// -------- Config --------
struct Config {
  String wifi_ssid;
  String wifi_pass;
  String device_name;
  bool   ap_fallback = true;
  bool   protect_web = true;
  String web_user    = "admin";
  String web_pass    = ""; // first-run: must be set via /setpass
};

Config cfg;

// ===== OLED helpers (U8g2) =====
static bool g_oled_ok = false;

void oledDrawLines(const String& l1, const String& l2, const String& l3) {
  if (!g_oled_ok) return;

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x12_tr);

  // 3 Zeilen (y = 14, 30, 46)
  u8g2.drawStr(0, 14, l1.c_str());
  u8g2.drawStr(0, 30, l2.c_str());
  u8g2.drawStr(0, 46, l3.c_str());

  u8g2.sendBuffer();
}

void oledBoot() {
  oledDrawLines(String(FW_NAME), "Boot...", " ");
}

void oledStatusConnecting(const String& ssid) {
  String s = ssid;
  if (s.length() > 20) s = s.substring(0, 20);
  oledDrawLines("Verbindung zum", "Router ...", s);
}

void oledStatusConnected(const IPAddress& ip) {
  oledDrawLines("Verbunden", "IP:", ip.toString());
}

void oledStatusAPActive(const IPAddress& ip) {
  oledDrawLines("Keine Verbindung", "AP aktiv", ip.toString());
}

void oledInit() {
  // Wunschpins setzen
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);

  // Viele OLEDs laufen stabiler mit 100kHz
  Wire.setClock(100000);

  // U8g2 init
  u8g2.begin();

  // Optional: Kontrast hoch (0..255)
  u8g2.setContrast(255);

  // Quick sanity draw
  g_oled_ok = true;
  oledBoot();
}

// -------- Helpers --------
String htmlEscape(const String& in) {
  String s = in;
  s.replace("&", "&amp;");
  s.replace("<", "&lt;");
  s.replace(">", "&gt;");
  s.replace("\"", "&quot;");
  s.replace("'", "&#39;");
  return s;
}

bool checkAuthIfNeeded() {
  if (!cfg.protect_web) return true;

  // First run: no password set yet -> force /setpass
  if (cfg.web_pass.length() == 0) {
    if (server.uri() != "/setpass") {
      server.sendHeader("Location", "/setpass", true);
      server.send(302, "text/plain", "First run: set web password at /setpass");
      return false;
    }
    return true;
  }

  if (server.authenticate(cfg.web_user.c_str(), cfg.web_pass.c_str())) return true;
  server.requestAuthentication();
  return false;
}

bool loadConfig() {
  if (!LittleFS.begin()) return false;
  if (!LittleFS.exists("/config.json")) return false;

  File f = LittleFS.open("/config.json", "r");
  if (!f) return false;

  StaticJsonDocument<512> doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) return false;

  cfg.wifi_ssid   = doc["wifi_ssid"]   | "";
  cfg.wifi_pass   = doc["wifi_pass"]   | "";
  cfg.device_name = doc["device_name"] | "esp8266";
  cfg.ap_fallback = doc["ap_fallback"] | true;
  cfg.protect_web = doc["protect_web"] | true;
  cfg.web_user    = doc["web_user"]    | "admin";
  cfg.web_pass    = doc["web_pass"]    | "";
  return true;
}

bool saveConfig() {
  if (!LittleFS.begin()) return false;

  StaticJsonDocument<512> doc;
  doc["wifi_ssid"]   = cfg.wifi_ssid;
  doc["wifi_pass"]   = cfg.wifi_pass;
  doc["device_name"] = cfg.device_name;
  doc["ap_fallback"] = cfg.ap_fallback;
  doc["protect_web"] = cfg.protect_web;
  doc["web_user"]    = cfg.web_user;
  doc["web_pass"]    = cfg.web_pass;

  File f = LittleFS.open("/config.json", "w");
  if (!f) return false;
  bool ok = (serializeJson(doc, f) > 0);
  f.close();
  return ok;
}

String currentIPString() {
  if (WiFi.getMode() & WIFI_STA) return WiFi.localIP().toString();
  return WiFi.softAPIP().toString();
}

// ===== WiFi =====
void startAP() {
  WiFi.mode(WIFI_AP);
  String apName = String(FW_NAME) + "-" + String(ESP.getChipId(), HEX);
  WiFi.softAP(apName.c_str(), "12345678");

  Serial.println();
  Serial.print("AP gestartet: ");
  Serial.println(apName);
  Serial.print("AP IP: ");
  Serial.println(WiFi.softAPIP());

  oledStatusAPActive(WiFi.softAPIP());
}

bool connectSTA(uint32_t timeoutMs = 15000) {
  if (cfg.wifi_ssid.length() == 0) return false;

  WiFi.mode(WIFI_STA);
  if (cfg.device_name.length() > 0) WiFi.hostname(cfg.device_name);

  oledStatusConnecting(cfg.wifi_ssid);

  WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
  Serial.print("Verbinde STA mit ");
  Serial.println(cfg.wifi_ssid);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - start) < timeoutMs) {
    delay(250);
    Serial.print(".");
    yield();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("STA verbunden, IP: ");
    Serial.println(WiFi.localIP());
    oledStatusConnected(WiFi.localIP());
    return true;
  }

  Serial.println("STA Verbindung fehlgeschlagen.");
  return false;
}

// -------- Pages --------
String pageHeader(const String& title) {
  String h;
  h += "<!doctype html><html><head><meta charset='utf-8'>";
  h += "<meta name='viewport' content='width=device-width, initial-scale=1'>";
  h += "<title>" + htmlEscape(title) + "</title>";
  h += "<style>";
  h += "body{font-family:system-ui,Segoe UI,Arial;margin:20px;max-width:780px}";
  h += "input,select{padding:10px;width:100%;box-sizing:border-box;margin:6px 0}";
  h += "button{padding:10px 14px;cursor:pointer}";
  h += "a{color:#06c;text-decoration:none} a:hover{text-decoration:underline}";
  h += ".card{border:1px solid #ddd;border-radius:12px;padding:14px;margin:12px 0}";
  h += ".row{display:flex;gap:12px;flex-wrap:wrap}";
  h += ".col{flex:1;min-width:220px}";
  h += ".small{color:#666;font-size:0.95rem}";
  h += "</style></head><body>";
  h += "<h2>" + String(FW_NAME) + " <span class='small'>v" + FW_VERSION + "</span></h2>";
  h += "<div class='small'>Build: " + String(FW_BUILD) + "</div>";
  return h;
}

String pageFooter() {
  return "<hr><div class='small'>/info, /config, /update</div></body></html>";
}

void handleRoot() {
  String ip = currentIPString();
  String mode = (WiFi.getMode() & WIFI_STA) ? "STA" : "AP";

  String html = pageHeader("Status");
  html += "<div class='card'>";
  html += "<b>Status</b><br>";
  html += "WiFi Mode: " + mode + "<br>";
  html += "IP: " + ip + "<br>";
  html += "SSID: " + htmlEscape(cfg.wifi_ssid) + "<br>";
  html += "RSSI: " + String(WiFi.RSSI()) + " dBm<br>";
  html += "Free heap: " + String(ESP.getFreeHeap()) + "<br>";
  html += "Chip ID: " + String(ESP.getChipId(), HEX) + "<br>";
  html += "Flash size: " + String(ESP.getFlashChipRealSize()) + "<br>";
  html += "</div>";

  html += "<div class='row'>";
  html += "<div class='card col'><b>Konfiguration</b><br><a href='/config'>/config öffnen</a></div>";
  html += "<div class='card col'><b>Firmware</b><br><a href='/update'>/update öffnen</a></div>";
  html += "</div>";

  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleInfo() {
  StaticJsonDocument<512> doc;
  doc["name"] = FW_NAME;
  doc["version"] = FW_VERSION;
  doc["build"] = FW_BUILD;
  doc["chip_id"] = String(ESP.getChipId(), HEX);
  doc["heap"] = ESP.getFreeHeap();
  doc["flash_real_size"] = ESP.getFlashChipRealSize();
  doc["wifi_mode"] = (WiFi.getMode() & WIFI_STA) ? "STA" : "AP";
  doc["ip"] = currentIPString();
  doc["rssi"] = WiFi.RSSI();
  doc["oled"] = g_oled_ok ? "ok" : "fail";
  doc["i2c_sda_gpio"] = OLED_SDA_PIN;
  doc["i2c_scl_gpio"] = OLED_SCL_PIN;
  doc["i2c_addr"] = "0x3C";

  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

void handleSetPassGet() {
  // No auth here: this is intentionally reachable when web_pass is empty.
  String html = pageHeader("First Run: Web-Passwort");
  html += "<div class='card'>";
  html += "<b>Erster Start</b><br>";
  html += "<div class='small'>Bitte setze jetzt ein Passwort f&uuml;r die Web-Oberfl&auml;che. Ohne Passwort sind /config und /update nicht erreichbar.</div>";
  html += "<form method='POST' action='/setpass'>";
  html += "<label>User</label><input name='user' value='" + htmlEscape(cfg.web_user.length() ? cfg.web_user : String("admin")) + "'>";
  html += "<label>Neues Passwort</label><input name='p1' type='password' required>";
  html += "<label>Passwort wiederholen</label><input name='p2' type='password' required>";
  html += "<button type='submit'>Passwort setzen</button>";
  html += "</form>";
  html += "<div class='small'>Empfehlung: mindestens 8 Zeichen.</div>";
  html += "</div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleSetPassPost() {
  String user = server.arg("user");
  String p1 = server.arg("p1");
  String p2 = server.arg("p2");

  user.trim();
  p1.trim();
  p2.trim();

  if (user.length() == 0) user = "admin";

  if (p1.length() < 8) {
    server.send(400, "text/plain", "Passwort zu kurz (min. 8 Zeichen).");
    return;
  }
  if (p1 != p2) {
    server.send(400, "text/plain", "Passworte stimmen nicht ueberein.");
    return;
  }

  cfg.protect_web = true;
  cfg.web_user = user;
  cfg.web_pass = p1;

  bool ok = saveConfig();
  if (!ok) {
    server.send(500, "text/plain", "Fehler beim Speichern (LittleFS).");
    return;
  }

  String html = pageHeader("OK");
  html += "<div class='card'><b>Passwort gesetzt.</b><br>";
  html += "Jetzt sind /config und /update geschuetzt.<br><br>";
  html += "<a href='/config'>Weiter zu /config</a>";
  html += "</div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleConfigGet() {
  if (!checkAuthIfNeeded()) return;

  String html = pageHeader("Config");
  html += "<div class='card'><b>WiFi & Gerät</b>";
  html += "<form method='POST' action='/config'>";
  html += "<label>SSID</label><input name='ssid' value='" + htmlEscape(cfg.wifi_ssid) + "'>";
  html += "<label>Passwort</label><input name='pass' type='password' value='' placeholder='(leer = unveraendert)'>";
  html += "<label>Device Name</label><input name='dev' value='" + htmlEscape(cfg.device_name) + "'>";
  html += "<label>AP Fallback</label><select name='apf'>";
  html += "<option value='1'" + String(cfg.ap_fallback ? " selected" : "") + ">an</option>";
  html += "<option value='0'" + String(!cfg.ap_fallback ? " selected" : "") + ">aus</option>";
  html += "</select>";
  html += "</div>";

  html += "<div class='card'><b>Web-Schutz (optional)</b>";
  html += "<label>Protect /config & /update</label><select name='prot'>";
  html += "<option value='1'" + String(cfg.protect_web ? " selected" : "") + ">an</option>";
  html += "<option value='0'" + String(!cfg.protect_web ? " selected" : "") + ">aus</option>";
  html += "</select>";
  html += "<label>User</label><input name='user' value='" + htmlEscape(cfg.web_user) + "'>";
  html += "<label>Password</label><input name='wp' type='password' value='' placeholder='(leer = unveraendert)'>";
  html += "</div>";

  html += "<button type='submit'>Speichern</button>";
  html += "</form>";

  html += "<div class='card'><b>Aktionen</b><br>";
  html += "<form method='POST' action='/reboot'><button type='submit'>Reboot</button></form>";
  html += "</div>";

  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleConfigPost() {
  if (!checkAuthIfNeeded()) return;

  cfg.wifi_ssid   = server.arg("ssid");
  String newWifiPass = server.arg("pass");
  if (newWifiPass.length()) cfg.wifi_pass = newWifiPass;
  cfg.device_name = server.arg("dev");
  cfg.ap_fallback = (server.arg("apf") == "1");
  cfg.protect_web = (server.arg("prot") == "1");
  cfg.web_user    = server.arg("user");
  String newWebPass = server.arg("wp");
  if (newWebPass.length()) cfg.web_pass = newWebPass;

  bool ok = saveConfig();
  server.send(200, "text/plain", ok ? "OK gespeichert. Bitte rebooten." : "Fehler beim Speichern.");
}

void handleReboot() {
  if (!checkAuthIfNeeded()) return;
  server.send(200, "text/plain", "Reboot...");
  delay(250);
  ESP.restart();
}

void handleUpdateGet() {
  if (!checkAuthIfNeeded()) return;

  String html = pageHeader("Firmware Update");
  html += "<div class='card'>";
  html += "<b>Firmware Upload</b><br>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='firmware' accept='.bin' required>";
  html += "<button type='submit'>Hochladen & Flashen</button>";
  html += "</form>";
  html += "<div class='small'>Hinweis: Nach Erfolg rebootet das Gerät automatisch.</div>";
  html += "</div>";
  html += pageFooter();
  server.send(200, "text/html", html);
}

void handleUpdatePost() {
  if (!checkAuthIfNeeded()) return;

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.setDebugOutput(true);
    Serial.printf("Update: %s\n", upload.filename.c_str());

    uint32_t maxSketchSpace = (ESP.getFreeSketchSpace() - 0x1000) & 0xFFFFF000;
    if (!Update.begin(maxSketchSpace)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (!Update.end(true)) {
      Update.printError(Serial);
    } else {
      Serial.printf("Update Success: %u bytes\n", upload.totalSize);
    }
    Serial.setDebugOutput(false);
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    Update.end();
    Serial.println("Update aborted");
  }
  yield();
}

void handleUpdateResult() {
  if (Update.hasError()) {
    server.send(500, "text/plain", "Update fehlgeschlagen.");
  } else {
    server.send(200, "text/plain", "Update OK. Reboot...");
    delay(250);
    ESP.restart();
  }
}

// -------- Setup / Loop --------
void setup() {
  Serial.begin(115200);
  delay(200);

  oledInit();   // << U8g2 OLED init

  loadConfig();

  bool connected = connectSTA();
  if (!connected && cfg.ap_fallback) {
    startAP();
  } else if (!connected) {
    oledDrawLines("Keine Verbindung", "AP fallback AUS", " ");
  }

  server.on("/", HTTP_GET, handleRoot);
  server.on("/info", HTTP_GET, handleInfo);

  server.on("/setpass", HTTP_GET, handleSetPassGet);
  server.on("/setpass", HTTP_POST, handleSetPassPost);

  server.on("/config", HTTP_GET, handleConfigGet);
  server.on("/config", HTTP_POST, handleConfigPost);

  server.on("/reboot", HTTP_POST, handleReboot);

  server.on("/update", HTTP_GET, handleUpdateGet);
  server.on("/update", HTTP_POST, handleUpdateResult, handleUpdatePost);

  server.begin();
  Serial.println("HTTP Server gestartet.");
}

void loop() {
  server.handleClient();
}
