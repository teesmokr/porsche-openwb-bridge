/*
 * openWB Porsche-Connect SoC-Bridge fuer ESP32
 * -------------------------------------------------------------------------
 * Holt den Ladestand (SoC), die Reichweite und den Kilometerstand eines
 * Porsche ueber Porsche Connect und stellt sie per HTTP bereit, damit openWB
 * sie mit dem eingebauten SoC-Modul "HTTP" abfragen kann.
 *
 * LOGIN: Du kannst dich direkt im Web-Interface anmelden (E-Mail/Passwort,
 * Captcha wird im Browser angezeigt) - dann braucht es das PC-Tool nicht.
 * Alternativ einen am PC erzeugten REFRESH-TOKEN eintragen. Der ESP32 erneuert
 * den Access-Token selbststaendig (rotierender Refresh-Token wird im Flash
 * gespeichert). Zusaetzlich: Online-Update (OTA) aus dem Git-Repo.
 *
 * Endpunkte fuer openWB:
 *   http://<ESP-IP>/soc     -> Ladestand als ganze Zahl  (soc_url)
 *   http://<ESP-IP>/range   -> Reichweite in km          (range_url)
 *   http://<ESP-IP>/status  -> JSON zum Pruefen
 *   http://<ESP-IP>/        -> Konfigurations-/Status-Webseite
 *
 * Benoetigte Bibliothek (Arduino IDE -> Bibliotheksverwalter):
 *   - ArduinoJson (v7)
 * ESP32-Boardpaket muss installiert sein (Espressif Systems).
 *
 * Login-Flow/Endpunkte portiert aus pyporscheconnectapi (Apache-2.0).
 * Inoffiziell - kann sich jederzeit aendern. Porsche-Connect-Abo noetig.
 */
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <map>

// Firmware-Version (fuer den Online-Updater)
static const char* FW_VERSION = "1.5.0";
// Standard-Update-Quelle (oeffentliches Firmware-Repo -> OTA ohne Token)
static const char* DEFAULT_UPDATE_URL =
  "https://raw.githubusercontent.com/teesmokr/porsche-openwb-firmware/main/version.json";

struct Resp;  // Forward-Declaration (Arduino generiert Prototypen vor der Definition)

// ---- Porsche-Connect-Konstanten -----------------------------------------
static const char* TOKEN_URL   = "https://identity.porsche.com/oauth/token";
static const char* API_BASE    = "https://api.ppa.porsche.com/app";
static const char* CLIENT_ID   = "XhygisuebbrqQ80byOuU5VncxLIm8E6H";
static const char* X_CLIENT_ID = "41843fb4-691d-4970-85c7-2673e8ecef40";
static const char* USER_AGENT  = "openWB-porsche-esp32/1.0";
static const char* REDIRECT_URI = "my-porsche-app://auth0/callback";
static const char* AUDIENCE    = "https://api.porsche.com";
static const char* SCOPE = "openid profile email offline_access mbb ssodb badge vin "
                           "dealers cars charging manageCharging plugAndCharge climatisation "
                           "manageClimatisation pid:user_profile.porscheid:read "
                           "pid:user_profile.name:read pid:user_profile.vehicles:read "
                           "pid:user_profile.emails:read pid:user_profile.locale:read";

// ---- Setup-Accesspoint (erster Start / kein WLAN) ------------------------
static const char* AP_SSID = "openWB-Porsche-Bridge";
static const char* AP_PASS = "porsche1234";  // >= 8 Zeichen

WebServer server(80);
DNSServer dnsServer;              // Captive Portal (leitet alle Domains auf den ESP)
Preferences prefs;

// ---- Konfiguration (aus Flash) ------------------------------------------
String cfgSsid, cfgPass, cfgRefresh, cfgVin;
uint32_t cfgIntervalMin = 10;
String cfgUpdateUrl, cfgUpdateToken;   // Online-Updater (version.json + optional Token)

// ---- OTA-Update-Status ---------------------------------------------------
String updateStatus = "";
String latestVersion = "";
String latestBinUrl = "";
bool   updateAvailable = false;

// ---- Web-Login (Auth0) Zwischenzustand -----------------------------------
std::map<String, String> authCookies;   // Cookie-Jar fuer den Login-Flow
String loginState;        // Auth0 'state'
String loginEmail, loginPassword;
String loginCaptcha;      // data-URI des Captcha-Bildes (fuer die Web-UI)
String loginError;        // Klartext-Fehler fuer die Web-UI
bool   loginNeedCaptcha = false;
bool   loginSuccess = false;

// ---- Laufzeit-Status -----------------------------------------------------
String accessToken;
uint32_t tokenExpiresAtMs = 0;
String resolvedVin;
int    curSoc   = -1;      // -1 = noch kein Wert
float  curRange = -1;
bool   curCharging = false;
String lastError = "";
uint32_t lastFetchMs = 0;
bool   apMode = false;
int    lastHttpCode = 0;
uint32_t bootMs = 0;

// ---- Log-Ringpuffer (fuers Troubleshooting im Web-UI) --------------------
#define LOG_LINES 14
String logBuf[LOG_LINES];
int    logHead = 0;

void logMsg(const String& m) {
  logBuf[logHead] = m;
  logHead = (logHead + 1) % LOG_LINES;
  Serial.println(m);
}

// ---- SoC-Verlauf (fuers Ladeverlauf-Diagramm) ----------------------------
#define HIST_N 60
int8_t socHist[HIST_N];
int histCount = 0, histHead = 0;
void pushHist(int v) {
  socHist[histHead] = (int8_t)v;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

// ==========================================================================
//  Konfiguration laden/speichern
// ==========================================================================
void loadConfig() {
  prefs.begin("porsche", true);
  cfgSsid    = prefs.getString("ssid", "");
  cfgPass    = prefs.getString("pass", "");
  cfgRefresh = prefs.getString("refresh", "");
  cfgVin     = prefs.getString("vin", "");
  cfgIntervalMin = prefs.getUInt("interval", 10);
  cfgUpdateUrl   = prefs.getString("upd_url", "");
  cfgUpdateToken = prefs.getString("upd_tok", "");
  if (cfgUpdateUrl.isEmpty()) cfgUpdateUrl = DEFAULT_UPDATE_URL;  // tokenlose OTA ab Werk
  prefs.end();
}

void saveRefreshToken(const String& rt) {
  cfgRefresh = rt;
  prefs.begin("porsche", false);
  prefs.putString("refresh", rt);
  prefs.end();
}

// ==========================================================================
//  WLAN
// ==========================================================================
bool connectWifi() {
  if (cfgSsid.isEmpty()) return false;
  WiFi.mode(WIFI_STA);
  WiFi.begin(cfgSsid.c_str(), cfgPass.c_str());
  Serial.printf("Verbinde mit WLAN '%s' ...\n", cfgSsid.c_str());
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(400);
    Serial.print(".");
  }
  Serial.println();
  return WiFi.status() == WL_CONNECTED;
}

void startAp() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1);
  WiFi.softAPConfig(apIP, apIP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(AP_SSID, AP_PASS);
  delay(200);
  // Captive Portal: alle DNS-Anfragen auf den ESP leiten -> Konfig oeffnet sich automatisch
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(53, "*", apIP);
  Serial.printf("Setup-AP aktiv: SSID '%s', Passwort '%s'\n", AP_SSID, AP_PASS);
  Serial.print("Konfiguration im Browser: http://");
  Serial.println(WiFi.softAPIP());
}

// ==========================================================================
//  Porsche: Token erneuern + SoC holen
// ==========================================================================
bool refreshAccessToken() {
  if (cfgRefresh.isEmpty()) { lastError = "Kein Refresh-Token gesetzt."; return false; }
  WiFiClientSecure client;
  client.setInsecure();  // TLS ohne Zertifikatspruefung (LAN-Geraet, einfach)
  HTTPClient https;
  if (!https.begin(client, TOKEN_URL)) { lastError = "TLS-Init fehlgeschlagen."; return false; }
  https.addHeader("Content-Type", "application/x-www-form-urlencoded");
  https.addHeader("User-Agent", USER_AGENT);
  https.addHeader("X-Client-ID", X_CLIENT_ID);
  String body = "client_id=" + String(CLIENT_ID) +
                "&grant_type=refresh_token&refresh_token=" + cfgRefresh;
  int code = https.POST(body);
  String payload = https.getString();
  https.end();
  lastHttpCode = code;
  if (code != 200) {
    lastError = "Token-Refresh HTTP " + String(code) +
                (code == 403 ? " (Refresh-Token ungueltig - am PC neu holen)." : "");
    logMsg(lastError);
    return false;
  }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { lastError = "Token-JSON nicht lesbar."; return false; }
  accessToken = doc["access_token"].as<String>();
  uint32_t expiresIn = doc["expires_in"] | 3600;
  tokenExpiresAtMs = millis() + (expiresIn - 60) * 1000UL;
  const char* newRt = doc["refresh_token"];
  if (newRt && strlen(newRt) > 0 && cfgRefresh != newRt) {
    saveRefreshToken(String(newRt));  // rotierenden Token merken
  }
  logMsg("Access-Token erneuert.");
  return true;
}

bool ensureToken() {
  if (!accessToken.isEmpty() && (int32_t)(tokenExpiresAtMs - millis()) > 0) return true;
  return refreshAccessToken();
}

String apiGet(const String& path, int& httpCode) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  String url = String(API_BASE) + path;
  if (!https.begin(client, url)) { httpCode = -1; return ""; }
  https.addHeader("Authorization", "Bearer " + accessToken);
  https.addHeader("User-Agent", USER_AGENT);
  https.addHeader("X-Client-ID", X_CLIENT_ID);
  httpCode = https.GET();
  lastHttpCode = httpCode;
  String payload = https.getString();
  https.end();
  return payload;
}

bool resolveVin() {
  if (!cfgVin.isEmpty()) { resolvedVin = cfgVin; resolvedVin.toUpperCase(); return true; }
  if (!resolvedVin.isEmpty()) return true;
  int code;
  String payload = apiGet("/connect/v1/vehicles", code);
  if (code != 200) { lastError = "Fahrzeugliste HTTP " + String(code); return false; }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { lastError = "Fahrzeugliste JSON-Fehler."; return false; }
  JsonArray arr = doc.is<JsonArray>() ? doc.as<JsonArray>() : doc["vehicles"].as<JsonArray>();
  if (arr.isNull() || arr.size() == 0) { lastError = "Keine Fahrzeuge im Konto."; return false; }
  resolvedVin = arr[0]["vin"].as<String>();
  return true;
}

void fetchSoc() {
  if (!ensureToken()) return;
  if (!resolveVin()) return;
  int code;
  String path = "/connect/v1/vehicles/" + resolvedVin +
                "?mf=BATTERY_LEVEL&mf=E_RANGE&mf=RANGE&mf=MILEAGE&mf=BATTERY_CHARGING_STATE";
  String payload = apiGet(path, code);
  if (code == 401) {                 // Token abgelaufen -> einmal erneuern
    if (!refreshAccessToken()) return;
    payload = apiGet(path, code);
  }
  if (code != 200) { lastError = "SoC-Abruf HTTP " + String(code); logMsg(lastError); return; }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { lastError = "SoC-JSON-Fehler."; logMsg(lastError); return; }
  int soc = -1; float range = -1; bool charging = false;
  for (JsonObject m : doc["measurements"].as<JsonArray>()) {
    const char* key = m["key"];
    if (!key) continue;
    if (strcmp(key, "BATTERY_LEVEL") == 0) {
      if (m["value"]["percent"].is<float>() || m["value"]["percent"].is<int>())
        soc = (int)round(m["value"]["percent"].as<float>());
    } else if (strcmp(key, "E_RANGE") == 0 || strcmp(key, "RANGE") == 0) {
      if (range < 0 && m["value"]["kilometers"].is<float>())
        range = m["value"]["kilometers"].as<float>();
    } else if (strcmp(key, "BATTERY_CHARGING_STATE") == 0) {
      // Feldname variiert -> defensiv nach "CHARG" in bekannten Feldern suchen
      const char* st = m["value"]["chargingState"] | (m["value"]["state"] | "");
      if (st && (strstr(st, "CHARG") || strstr(st, "charg"))) charging = true;
    }
  }
  if (soc < 0) { lastError = "Kein BATTERY_LEVEL erhalten."; logMsg(lastError); return; }
  curSoc = soc; curRange = range; curCharging = charging; lastError = ""; lastFetchMs = millis();
  pushHist(soc);
  logMsg("SoC aktualisiert: " + String(soc) + " %" +
         (range < 0 ? "" : ", " + String(range, 0) + " km"));
}

// ==========================================================================
//  Web-Interface
// ==========================================================================
// ==========================================================================
//  Online-Updater (OTA aus dem Git-Repo)
// ==========================================================================
bool httpGetString(const String& url, const String& token, String& out, int& code) {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) { code = -1; return false; }
  https.addHeader("User-Agent", "openWB-porsche-esp32");
  https.addHeader("Accept", "application/vnd.github.raw");
  if (token.length()) https.addHeader("Authorization", "token " + token);
  code = https.GET();
  out = https.getString();
  https.end();
  return code == 200;
}

void checkUpdate() {
  if (cfgUpdateUrl.isEmpty()) { updateStatus = "Keine Update-URL konfiguriert."; return; }
  String body; int code;
  if (!httpGetString(cfgUpdateUrl, cfgUpdateToken, body, code)) {
    updateStatus = "Update-Pruefung HTTP " + String(code);
    logMsg(updateStatus); return;
  }
  JsonDocument d;
  if (deserializeJson(d, body)) { updateStatus = "version.json nicht lesbar."; return; }
  latestVersion = d["version"].as<String>();
  latestBinUrl  = d["bin"].as<String>();
  updateAvailable = (latestVersion.length() && latestVersion != FW_VERSION);
  updateStatus = updateAvailable ? ("Update verfuegbar: " + latestVersion + " (installiert: " + FW_VERSION + ")")
                                 : ("Firmware aktuell (" + String(FW_VERSION) + ").");
  logMsg(updateStatus);
}

void doUpdate() {
  if (latestBinUrl.isEmpty()) { updateStatus = "Erst 'Auf Updates pruefen'."; return; }
  updateStatus = "Update laeuft, bitte warten ...";
  logMsg("OTA-Update von " + latestBinUrl);
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  http.begin(client, latestBinUrl);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("User-Agent", "openWB-porsche-esp32");
  if (cfgUpdateToken.length()) http.addHeader("Authorization", "token " + cfgUpdateToken);
  httpUpdate.rebootOnUpdate(true);
  t_httpUpdate_return ret = httpUpdate.update(http);
  http.end();
  if (ret == HTTP_UPDATE_FAILED) {
    updateStatus = "Update fehlgeschlagen: " + httpUpdate.getLastErrorString();
    logMsg(updateStatus);
  } else if (ret == HTTP_UPDATE_NO_UPDATES) {
    updateStatus = "Keine Aktualisierung noetig.";
  }  // bei Erfolg rebootet das Geraet automatisch
}

// ==========================================================================
//  Web-Login gegen Porsche/Auth0 (inkl. Captcha) - EXPERIMENTELL
// ==========================================================================
String urlencode(const String& s) {
  String o; char b[4];
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += c;
    else { sprintf(b, "%%%02X", (uint8_t)c); o += b; }
  }
  return o;
}

void storeCookie(const String& setCookie) {
  int eq = setCookie.indexOf('=');
  if (eq <= 0) return;
  int sc = setCookie.indexOf(';');
  String name = setCookie.substring(0, eq); name.trim();
  String val = setCookie.substring(eq + 1, sc < 0 ? setCookie.length() : sc);
  authCookies[name] = val;
}

String cookieHeaderLine() {
  if (authCookies.empty()) return "";
  String s = "Cookie: ";
  bool first = true;
  for (auto& kv : authCookies) { if (!first) s += "; "; s += kv.first + "=" + kv.second; first = false; }
  s += "\r\n";
  return s;
}

struct Resp { int status; String location; String body; };

bool rawHttps(const String& host, const String& method, const String& path,
              const String& extraHeaders, const String& reqBody, Resp& r) {
  r.status = 0; r.location = ""; r.body = "";
  WiFiClientSecure c; c.setInsecure(); c.setTimeout(15);
  if (!c.connect(host.c_str(), 443)) { loginError = "Verbindung zu " + host + " fehlgeschlagen."; return false; }
  String req = method + " " + path + " HTTP/1.1\r\n";
  req += "Host: " + host + "\r\n";
  req += "User-Agent: " + String(USER_AGENT) + "\r\n";
  req += "X-Client-ID: " + String(X_CLIENT_ID) + "\r\n";
  req += "Accept: */*\r\n";
  req += "Connection: close\r\n";
  req += cookieHeaderLine();
  req += extraHeaders;
  if (method == "POST") req += "Content-Length: " + String(reqBody.length()) + "\r\n";
  req += "\r\n" + reqBody;
  c.print(req);

  String statusLine = c.readStringUntil('\n');
  int sp = statusLine.indexOf(' ');
  if (sp < 0) { c.stop(); return false; }
  r.status = statusLine.substring(sp + 1, sp + 4).toInt();

  bool chunked = false; long contentLen = -1;
  while (true) {
    String line = c.readStringUntil('\n');
    line.replace("\r", "");
    if (line.length() == 0) break;
    int colon = line.indexOf(':');
    if (colon < 0) continue;
    String name = line.substring(0, colon); String val = line.substring(colon + 1); val.trim();
    if (name.equalsIgnoreCase("Location")) r.location = val;
    else if (name.equalsIgnoreCase("Set-Cookie")) storeCookie(val);
    else if (name.equalsIgnoreCase("Transfer-Encoding") && val.indexOf("chunked") >= 0) chunked = true;
    else if (name.equalsIgnoreCase("Content-Length")) contentLen = val.toInt();
  }

  const size_t CAP = 100000;  // Body-Obergrenze gegen OOM
  uint32_t t0 = millis();
  if (chunked) {
    while (millis() - t0 < 15000) {
      String len = c.readStringUntil('\n'); len.replace("\r", ""); len.trim();
      long n = strtol(len.c_str(), nullptr, 16);
      if (n <= 0) break;
      while (n > 0 && (c.connected() || c.available())) {
        if (c.available()) { char ch = c.read(); if (r.body.length() < CAP) r.body += ch; n--; }
      }
      c.readStringUntil('\n');  // CRLF nach Chunk
    }
  } else if (contentLen > 0) {
    long n = contentLen;
    while (n > 0 && (c.connected() || c.available()) && millis() - t0 < 15000) {
      if (c.available()) { char ch = c.read(); if (r.body.length() < CAP) r.body += ch; n--; }
    }
  } else {
    while ((c.connected() || c.available()) && millis() - t0 < 15000) {
      if (c.available()) { char ch = c.read(); if (r.body.length() < CAP) r.body += ch; }
    }
  }
  c.stop();
  return true;
}

String extractCaptcha(const String& html) {
  int i = html.indexOf("\"image\":\"data:image");
  if (i >= 0) {
    int s = html.indexOf("data:image", i);
    int e = html.indexOf('"', s);
    if (e > s) { String u = html.substring(s, e); u.replace("\\/", "/"); return u; }
  }
  i = html.indexOf("src=\"data:image");
  if (i >= 0) {
    int s = html.indexOf("data:image", i);
    int e = html.indexOf('"', s);
    if (e > s) return html.substring(s, e);
  }
  return "";
}

bool exchangeCodeWeb(const String& code) {
  Resp r;
  String body = "client_id=" + String(CLIENT_ID) + "&grant_type=authorization_code&code=" +
                urlencode(code) + "&redirect_uri=" + urlencode(REDIRECT_URI);
  if (!rawHttps("identity.porsche.com", "POST", "/oauth/token",
                "Content-Type: application/x-www-form-urlencoded\r\n", body, r)) return false;
  if (r.status != 200) { loginError = "Token-Tausch HTTP " + String(r.status); return false; }
  JsonDocument d;
  if (deserializeJson(d, r.body)) { loginError = "Token-Antwort nicht lesbar."; return false; }
  const char* rt = d["refresh_token"];
  if (!rt || strlen(rt) == 0) { loginError = "Kein Refresh-Token erhalten."; return false; }
  saveRefreshToken(String(rt));
  accessToken = d["access_token"].as<String>();
  tokenExpiresAtMs = millis() + ((uint32_t)(d["expires_in"] | 3600) - 60) * 1000UL;
  loginSuccess = true; loginNeedCaptcha = false; loginError = "";
  logMsg("Web-Login erfolgreich, Refresh-Token gespeichert.");
  return true;
}

// identifier -> (captcha?) -> password -> resume -> code -> token
bool doIdentifierAndFinish(const String& captchaCode) {
  Resp r;
  String body = "state=" + urlencode(loginState) + "&username=" + urlencode(loginEmail) +
                "&js-available=true&webauthn-available=false&is-brave=false" +
                "&webauthn-platform-available=false&action=default";
  if (captchaCode.length()) body += "&captcha=" + urlencode(captchaCode);
  if (!rawHttps("identity.porsche.com", "POST", "/u/login/identifier?state=" + urlencode(loginState),
                "Content-Type: application/x-www-form-urlencoded\r\n", body, r)) return false;
  if (r.status == 401) { loginError = "E-Mail wurde abgelehnt."; return false; }
  if (r.status == 400) {
    loginCaptcha = extractCaptcha(r.body);
    loginNeedCaptcha = true;
    loginError = captchaCode.length() ? "Captcha war falsch, bitte erneut." : "Captcha erforderlich.";
    if (loginCaptcha.isEmpty()) loginError = "Captcha noetig, Bild nicht lesbar.";
    return false;
  }
  // Passwort
  Resp p;
  String pbody = "state=" + urlencode(loginState) + "&username=" + urlencode(loginEmail) +
                 "&password=" + urlencode(loginPassword) + "&action=default";
  if (!rawHttps("identity.porsche.com", "POST", "/u/login/password?state=" + urlencode(loginState),
                "Content-Type: application/x-www-form-urlencoded\r\n", pbody, p)) return false;
  if (p.status == 400) { loginError = "Passwort wurde abgelehnt."; return false; }
  if (p.location.isEmpty()) { loginError = "Login-Schritt Passwort: Status " + String(p.status); return false; }
  delay(2500);
  // Resume -> code
  String resumePath = p.location;
  if (resumePath.startsWith("http")) { int q = resumePath.indexOf('/', 8); resumePath = resumePath.substring(q); }
  Resp res;
  if (!rawHttps("identity.porsche.com", "GET", resumePath, "", "", res)) return false;
  int ci = res.location.indexOf("code=");
  if (ci < 0) { loginError = "Kein Authorization-Code nach Login."; return false; }
  String code = res.location.substring(ci + 5);
  int amp = code.indexOf('&'); if (amp >= 0) code = code.substring(0, amp);
  return exchangeCodeWeb(code);
}

void startWebLogin(const String& email, const String& password) {
  authCookies.clear();
  loginEmail = email; loginPassword = password;
  loginError = ""; loginCaptcha = ""; loginNeedCaptcha = false; loginSuccess = false;
  if (email.isEmpty() || password.isEmpty()) { loginError = "E-Mail und Passwort noetig."; return; }
  String q = "/authorize?response_type=code&client_id=" + String(CLIENT_ID) +
             "&redirect_uri=" + urlencode(REDIRECT_URI) + "&audience=" + urlencode(AUDIENCE) +
             "&scope=" + urlencode(SCOPE) + "&state=openwb";
  Resp r;
  if (!rawHttps("identity.porsche.com", "GET", q, "", "", r)) return;
  if (r.status != 302 || r.location.isEmpty()) { loginError = "Auth0 /authorize: Status " + String(r.status); return; }
  int ci = r.location.indexOf("code=");
  if (ci >= 0) {  // bereits eingeloggt
    String code = r.location.substring(ci + 5); int amp = code.indexOf('&'); if (amp >= 0) code = code.substring(0, amp);
    exchangeCodeWeb(code); return;
  }
  int si = r.location.indexOf("state=");
  if (si < 0) { loginError = "Kein 'state' von Auth0."; return; }
  loginState = r.location.substring(si + 6);
  int amp = loginState.indexOf('&'); if (amp >= 0) loginState = loginState.substring(0, amp);
  doIdentifierAndFinish("");
}

const char PAGE[] PROGMEM = R"HTML(
<!doctype html><html lang="de"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>openWB Porsche Bridge</title><style>
:root{--bg:#eef0f3;--surf:#fff;--surf2:#fff;--line:#e3e5ea;--fg:#14151a;--mut:#6c6f7a;
--red:#d5001c;--red2:#ff2038;--ok:#12a150;--warn:#c67a00;--err:#d5001c;--track:#e6e8ee;--shadow:0 1px 3px rgba(20,20,30,.08)}
:root.dark{--bg:#0a0a0c;--surf:#111114;--surf2:#17171c;--line:#2a2a31;--fg:#f4f4f6;
--mut:#8b8b93;--track:#232329;--shadow:none}
*{box-sizing:border-box}html,body{margin:0;overflow-x:hidden;max-width:100%}
.urlbox code{min-width:0}
body{background:var(--bg);color:var(--fg);font-family:"Helvetica Neue",Arial,system-ui,sans-serif;
-webkit-font-smoothing:antialiased;transition:background .3s,color .3s;overflow-x:hidden}
svg{max-width:100%}
.wrap{max-width:760px;margin:0 auto;padding:0 16px 40px}
.ic{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:2;stroke-linecap:round;stroke-linejoin:round}
.top{display:flex;align-items:center;justify-content:space-between;padding:18px 2px}
.brand{display:flex;align-items:center;gap:10px;font-weight:700;font-size:17px;letter-spacing:.02em}
.brand .bdot{color:var(--red)}
.tools{display:flex;gap:8px}
.iconbtn{width:42px;height:42px;border-radius:12px;border:1px solid var(--line);background:var(--surf);
color:var(--fg);display:flex;align-items:center;justify-content:center;cursor:pointer;box-shadow:var(--shadow)}
.iconbtn:hover{border-color:var(--red)}
.card{background:var(--surf);border:1px solid var(--line);border-radius:18px;padding:20px;margin:14px 0;box-shadow:var(--shadow)}
.ctitle{display:flex;align-items:center;gap:10px;font-size:15px;font-weight:700;margin:0 0 4px}
.ctitle .ic{color:var(--red);width:19px;height:19px}
.sub{color:var(--mut);font-size:13.5px;line-height:1.5}
.hero{display:flex;gap:24px;align-items:center;flex-wrap:wrap}
.gaugewrap{position:relative;width:172px;height:172px;flex:none;margin:0 auto}
.gauge{transform:rotate(-90deg)}
.g-track{fill:none;stroke:var(--track);stroke-width:10}
.g-val{fill:none;stroke:url(#grad);stroke-width:10;stroke-linecap:round;transition:stroke-dashoffset 1s ease}
.g-center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.g-center b{font-size:50px;font-weight:800;line-height:1;letter-spacing:-.02em}
.g-center .lab{font-size:12px;color:var(--mut);margin-top:4px;text-transform:uppercase;letter-spacing:.14em}
.bolt{position:absolute;top:16px;left:50%;transform:translateX(-50%);color:var(--red);display:none}
.bolt .ic{width:22px;height:22px;fill:var(--red);stroke:none}
.hstats{flex:1;min-width:200px}
.tile{display:flex;align-items:center;gap:12px;padding:12px 0;border-bottom:1px solid var(--line)}
.tile:last-child{border-bottom:0}
.tile .ic{color:var(--mut);flex:none}
.tile .k{color:var(--mut);font-size:12px}.tile .v{font-size:17px;font-weight:700}
.tile .tv{margin-left:auto;text-align:right}
.pill{display:inline-flex;align-items:center;gap:8px;padding:9px 15px;border-radius:999px;
font-weight:700;font-size:14px;margin-top:4px}
.pill .ic{width:18px;height:18px}
.pill.ok{background:rgba(18,161,80,.12);color:var(--ok)}
.pill.warn{background:rgba(198,122,0,.14);color:var(--warn)}
.pill.err{background:rgba(213,0,28,.12);color:var(--err)}
label{display:block;margin:14px 0 6px;font-size:13px;font-weight:600;color:var(--fg)}
input,select{width:100%;padding:12px 13px;border:1px solid var(--line);border-radius:12px;
background:var(--bg);color:var(--fg);font-size:15px;outline:none;font-family:inherit}
input:focus,select:focus{border-color:var(--red);background:var(--surf)}
.btn{display:inline-flex;align-items:center;gap:8px;font-family:inherit;border:0;border-radius:12px;
padding:12px 18px;font-size:14px;font-weight:700;cursor:pointer;margin-top:12px}
.btn .ic{width:18px;height:18px}
.primary{background:var(--red);color:#fff}.primary:hover{background:var(--red2)}
.ghost{background:transparent;color:var(--fg);border:1px solid var(--line)}.ghost:hover{border-color:var(--red)}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.urlbox{display:flex;align-items:center;gap:10px;margin-top:12px;background:var(--bg);
border:1px solid var(--line);border-radius:12px;padding:6px 6px 6px 14px}
.urlbox code{flex:1;font-size:13px;word-break:break-all;background:none;border:0;padding:0;color:var(--fg)}
.hint{display:flex;gap:8px;align-items:flex-start;color:var(--warn);font-size:13.5px;margin-top:10px}
.metric{flex:1;min-width:120px;padding:8px 0}
.metric .k{color:var(--mut);font-size:12px}.metric .v{font-size:16px;font-weight:700}
.log{font-family:"SF Mono",Consolas,monospace;font-size:12px;background:var(--bg);border:1px solid var(--line);
border-radius:12px;padding:12px;max-height:170px;overflow:auto;white-space:pre-wrap;color:var(--mut);margin-top:12px}
.foot{color:var(--mut);font-size:12px;margin-top:22px;text-align:center}
#capimg{max-width:240px;background:#fff;border-radius:10px;margin:10px 0;display:block;padding:6px}
.link{color:var(--red);cursor:pointer;font-weight:600;font-size:13.5px}
.adminhead{display:flex;align-items:center;gap:12px;padding:18px 2px}
.chart{width:100%;height:100px;display:block}
.chart .line{fill:none;stroke:var(--red);stroke-width:2.5;stroke-linejoin:round;stroke-linecap:round}
.chart .area{fill:url(#agrad);opacity:.2}
.chart .grid{stroke:var(--line);stroke-width:1}
.chartlab{display:flex;justify-content:space-between;color:var(--mut);font-size:11px;margin-top:6px}
.swatches{display:flex;gap:12px;margin-top:10px}
.sw{width:34px;height:34px;border-radius:50%;cursor:pointer;border:2px solid var(--line);box-shadow:var(--shadow)}
.sw.active{border-color:var(--fg);transform:scale(1.08)}
@media(max-width:560px){
.wrap{padding:0 12px 32px}.card{padding:16px;border-radius:16px}
.g-center b{font-size:44px}.gaugewrap{width:150px;height:150px;margin:0 auto}
.hero{gap:14px;flex-direction:column;align-items:stretch}.hstats{min-width:0}
.btn{width:100%;justify-content:center}.iconbtn{width:46px;height:46px}
.tile{min-width:0}.tile .v,.tile .tv{font-size:15px;min-width:0;word-break:break-word}}
</style></head><body>
<svg width="0" height="0" style="position:absolute"><defs>
<symbol id="i-sun" viewBox="0 0 24 24"><circle cx="12" cy="12" r="4"/><path d="M12 2v2M12 20v2M4.9 4.9l1.4 1.4M17.7 17.7l1.4 1.4M2 12h2M20 12h2M4.9 19.1l1.4-1.4M17.7 6.3l1.4-1.4"/></symbol>
<symbol id="i-moon" viewBox="0 0 24 24"><path d="M21 12.8A9 9 0 1 1 11.2 3 7 7 0 0 0 21 12.8z"/></symbol>
<symbol id="i-cog" viewBox="0 0 24 24"><path d="M4 21v-7M4 10V3M12 21v-9M12 8V3M20 21v-5M20 12V3M1 14h6M9 8h6M17 16h6"/></symbol>
<symbol id="i-back" viewBox="0 0 24 24"><path d="M19 12H5M12 19l-7-7 7-7"/></symbol>
<symbol id="i-route" viewBox="0 0 24 24"><circle cx="6" cy="19" r="2"/><circle cx="18" cy="5" r="2"/><path d="M8 19h6a4 4 0 0 0 0-8H10a4 4 0 0 1 0-8h6"/></symbol>
<symbol id="i-clock" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M12 7v5l3 2"/></symbol>
<symbol id="i-car" viewBox="0 0 24 24"><path d="M5 13l1.6-4.8A2 2 0 0 1 8.5 7h7a2 2 0 0 1 1.9 1.2L19 13M5 13h14v4H5zM7 17v2M17 17v2"/><circle cx="7.5" cy="15" r="1"/><circle cx="16.5" cy="15" r="1"/></symbol>
<symbol id="i-wifi" viewBox="0 0 24 24"><path d="M5 12.5a10 10 0 0 1 14 0M8.5 16a5 5 0 0 1 7 0"/><circle cx="12" cy="19" r="1" fill="currentColor"/></symbol>
<symbol id="i-bolt" viewBox="0 0 24 24"><path d="M13 2 3 14h8l-1 8 11-13h-8l0-7z"/></symbol>
<symbol id="i-refresh" viewBox="0 0 24 24"><path d="M21 12a9 9 0 1 1-2.6-6.3M21 4v5h-5"/></symbol>
<symbol id="i-download" viewBox="0 0 24 24"><path d="M12 3v12M7 10l5 5 5-5M5 21h14"/></symbol>
<symbol id="i-check" viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><path d="M8.5 12.5l2.5 2.5 4.5-5"/></symbol>
<symbol id="i-alert" viewBox="0 0 24 24"><path d="M12 9v4M12 17h.01M10.3 3.9 1.8 18a2 2 0 0 0 1.7 3h17a2 2 0 0 0 1.7-3L13.7 3.9a2 2 0 0 0-3.4 0z"/></symbol>
<symbol id="i-copy" viewBox="0 0 24 24"><rect x="9" y="9" width="11" height="11" rx="2"/><path d="M5 15V5a2 2 0 0 1 2-2h10"/></symbol>
<symbol id="i-link" viewBox="0 0 24 24"><path d="M10 13a5 5 0 0 0 7 0l3-3a5 5 0 0 0-7-7l-1 1M14 11a5 5 0 0 0-7 0l-3 3a5 5 0 0 0 7 7l1-1"/></symbol>
<symbol id="i-key" viewBox="0 0 24 24"><circle cx="8" cy="15" r="4"/><path d="M10.8 12.2 20 3M17 6l3 3M15 8l2 2"/></symbol>
<symbol id="i-wave" viewBox="0 0 24 24"><path d="M2 12s3-6 10-6 10 6 10 6"/><circle cx="12" cy="13" r="3"/></symbol>
<symbol id="i-wrench" viewBox="0 0 24 24"><path d="M14.7 6.3a4 4 0 0 0-5.4 5.2L3 18v3h3l6.5-6.3a4 4 0 0 0 5.2-5.4l-2.5 2.5-2.3-2.3 2.5-2.5z"/></symbol>
<symbol id="i-chart" viewBox="0 0 24 24"><path d="M3 3v18h18"/><path d="M7 14l4-5 3 3 5-7"/></symbol>
</defs></svg>

<div class="wrap">
<div class="top"><div class="brand">openWB <span class="bdot">&middot;</span> SoC</div>
<div class="tools">
<button class="iconbtn" onclick="toggleTheme()" title="Hell/Dunkel"><svg class="ic"><use id="themeicon" href="#i-moon"/></svg></button>
<button class="iconbtn" onclick="showAdmin()" title="Einstellungen"><svg class="ic"><use href="#i-cog"/></svg></button>
</div></div>

<!-- ================= DASHBOARD ================= -->
<div id="view-dash">
<div class="card"><div class="hero">
<div class="gaugewrap">
<div class="bolt" id="bolt"><svg class="ic"><use href="#i-bolt"/></svg></div>
<svg class="gauge" width="172" height="172" viewBox="0 0 120 120">
<defs><linearGradient id="grad" x1="0" y1="0" x2="1" y2="1">
<stop id="gs1" offset="0" stop-color="#ff2038"/><stop id="gs2" offset="1" stop-color="#d5001c"/></linearGradient></defs>
<circle class="g-track" cx="60" cy="60" r="52"/><circle id="ring" class="g-val" cx="60" cy="60" r="52"/></svg>
<div class="g-center"><b id="soc">&ndash;</b><span class="lab">Ladestand</span></div></div>
<div class="hstats">
<div class="pill" id="pill"><svg class="ic"><use id="pillicon" href="#i-clock"/></svg><span id="statustext">...</span></div>
<div class="tile"><svg class="ic"><use href="#i-route"/></svg><span class="k">Reichweite</span><span class="v tv" id="range">&ndash;</span></div>
<div class="tile"><svg class="ic"><use href="#i-clock"/></svg><span class="k">Aktualisiert</span><span class="v tv" id="age">&ndash;</span></div>
<div class="tile"><svg class="ic"><use href="#i-car"/></svg><span class="k">Fahrzeug</span><span class="tv" id="vinv" style="font-weight:700;font-size:13px">&ndash;</span></div>
</div></div>
<button class="btn primary" onclick="act('refresh')"><svg class="ic"><use href="#i-refresh"/></svg>Jetzt aktualisieren</button>
</div>

<div class="card" id="chartcard" style="display:none">
<div class="ctitle"><svg class="ic"><use href="#i-chart"/></svg>Ladeverlauf</div>
<svg class="chart" id="chart" viewBox="0 0 300 100" preserveAspectRatio="none">
<defs><linearGradient id="agrad" x1="0" y1="0" x2="0" y2="1">
<stop id="ags1" offset="0" stop-color="#d5001c"/><stop offset="1" stop-color="#d5001c" stop-opacity="0"/></linearGradient></defs>
<line class="grid" x1="0" y1="10" x2="300" y2="10"/><line class="grid" x1="0" y1="90" x2="300" y2="90"/>
<path class="area" id="charea" d=""/><path class="line" id="chline" d=""/></svg>
<div class="chartlab"><span id="chspan">Verlauf</span><span>0&ndash;100 %</span></div></div>
<div class="card" id="connectcard">
<div class="ctitle"><svg class="ic"><use href="#i-key"/></svg><span id="connecttitle">Mit Porsche verbinden</span></div>
<div class="sub" id="connectednote" style="display:none">Verbunden mit deinem Porsche-Konto. <span class="link" onclick="showLogin()">Neu anmelden</span></div>
<div id="loginform">
<div class="sub">Melde dich mit deiner Porsche&nbsp;ID an &ndash; direkt hier, kein PC noetig.</div>
<label>E-Mail</label><input id="lmail" placeholder="deine@porsche-id.de">
<label>Passwort</label><input id="lpass" type="password">
<button class="btn primary" onclick="doLogin()">Anmelden</button>
<div id="capbox" style="display:none;margin-top:12px">
<div class="sub">Bitte die Zeichen aus dem Bild eingeben:</div>
<img id="capimg"><input id="capcode" placeholder="Code aus dem Bild" style="max-width:240px">
<button class="btn primary" onclick="doCaptcha()">Weiter</button></div>
<div id="lstat" class="hint" style="display:none"><svg class="ic"><use href="#i-alert"/></svg><span id="lstattext"></span></div>
</div></div>

<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-link"/></svg>So verbindest du openWB</div>
<div class="sub">Trage diese Adresse in openWB ein bei <b>Fahrzeug &rarr; SoC-Modul &bdquo;HTTP&ldquo;</b>:</div>
<div class="urlbox"><code id="usoc"></code>
<button class="iconbtn" style="width:38px;height:38px" onclick="cp('usoc')" title="Kopieren"><svg class="ic"><use href="#i-copy"/></svg></button></div>
<div class="sub" style="margin-top:8px">Reichweite (optional): <span class="link" onclick="cp('urange')">Adresse kopieren</span>
<span id="urange" style="display:none"></span></div></div>
</div>

<!-- ================= ADMIN ================= -->
<div id="view-admin" style="display:none">
<div class="adminhead"><button class="iconbtn" onclick="showDash()"><svg class="ic"><use href="#i-back"/></svg></button>
<div style="font-weight:700;font-size:17px">Einstellungen</div></div>

<div class="card" id="setuphint" style="display:none;border-color:var(--red)">
<div class="ctitle"><svg class="ic"><use href="#i-wifi"/></svg>Willkommen! Erst-Einrichtung</div>
<div class="sub">Gib unten dein <b>WLAN</b> ein und speichere. Die Bridge startet neu und
verbindet sich mit deinem Netz. Danach meldest du dich mit deiner Porsche&nbsp;ID an.</div></div>
<form method="POST" action="/save">
<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-wifi"/></svg>Netzwerk</div>
<label>WLAN-Name (SSID)</label><input name="ssid" id="fssid">
<label>WLAN-Passwort</label><input name="pass" type="password" placeholder="(leer = unveraendert)">
</div>
<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-car"/></svg>Fahrzeug &amp; Abruf</div>
<label>VIN (nur bei mehreren Fahrzeugen)</label><input name="vin" id="fvin">
<label>Porsche-Abruf-Intervall</label>
<select name="interval" id="fint">
<option value="5">alle 5 Minuten</option>
<option value="10">alle 10 Minuten (empfohlen)</option>
<option value="15">alle 15 Minuten</option>
<option value="30">alle 30 Minuten</option>
<option value="60">stuendlich</option>
</select>
<div class="sub" style="margin-top:6px">Kuerzer als 5 Min. ist gesperrt, damit Porsche dich nicht wegen zu vieler Anfragen blockiert. openWB liest den Ladestand jederzeit vom ESP32 &ndash; unabhaengig davon.</div>
<label>Refresh-Token (optional, statt Login)</label><input name="refresh" placeholder="hier einfuegen zum Aendern">
</div>
<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-download"/></svg>Update-Quelle</div>
<label>Update-URL</label><input name="update_url" id="fupd">
<label>Update-Token (nur privates Repo)</label><input name="update_token" type="password" placeholder="(leer = unveraendert)">
</div>
<button class="btn primary" type="submit"><svg class="ic"><use href="#i-check"/></svg>Speichern &amp; neu starten</button>
</form>

<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-download"/></svg>Firmware-Update</div>
<div class="sub">Installiert: v<span id="fw">?</span></div>
<div id="ustat" class="sub" style="margin-top:6px"></div>
<div class="row"><button class="btn ghost" onclick="checkUpd()"><svg class="ic"><use href="#i-refresh"/></svg>Auf Updates pruefen</button>
<button class="btn primary" id="ubtn" style="display:none" onclick="doUpd()"><svg class="ic"><use href="#i-download"/></svg>Installieren</button></div></div>

<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-wrench"/></svg>Diagnose</div>
<div class="row" style="margin-top:6px">
<div class="metric"><div class="k">IP-Adresse</div><div class="v" id="dip">&ndash;</div></div>
<div class="metric"><div class="k">WLAN-Signal</div><div class="v" id="drssi">&ndash;</div></div>
<div class="metric"><div class="k">Letzter Status</div><div class="v" id="dcode">&ndash;</div></div>
<div class="metric"><div class="k">Laufzeit</div><div class="v" id="dup">&ndash;</div></div></div>
<div class="log" id="log">...</div></div>
<div class="card">
<div class="ctitle"><svg class="ic"><use href="#i-sun"/></svg>Darstellung</div>
<div class="sub">Akzentfarbe (Hell/Dunkel-Umschalter ist oben rechts)</div>
<div class="swatches" id="swatches">
<div class="sw" data-c="#d5001c" data-c2="#ff2038" style="background:#d5001c" onclick="setAccent(this)"></div>
<div class="sw" data-c="#0a7cff" data-c2="#3d9bff" style="background:#0a7cff" onclick="setAccent(this)"></div>
<div class="sw" data-c="#12a150" data-c2="#1bd06a" style="background:#12a150" onclick="setAccent(this)"></div>
<div class="sw" data-c="#f0a020" data-c2="#ffbe4d" style="background:#f0a020" onclick="setAccent(this)"></div>
<div class="sw" data-c="#8a8f98" data-c2="#a7acb5" style="background:#8a8f98" onclick="setAccent(this)"></div>
</div></div>
</div>

<div class="foot">Inoffizielles Tool &middot; Porsche-Connect-Abo noetig</div>
</div>
<script>
var RC=2*Math.PI*52, forceLogin=false;
function T(id){return document.getElementById(id)}
function icon(id,name){T(id).setAttribute('href','#'+name)}
function toggleTheme(){var d=document.documentElement.classList.toggle('dark');
localStorage.thm=d?'dark':'light';icon('themeicon',d?'i-sun':'i-moon')}
(function(){var t=localStorage.thm||(matchMedia('(prefers-color-scheme: dark)').matches?'dark':'light');
if(t==='dark')document.documentElement.classList.add('dark');
document.addEventListener('DOMContentLoaded',()=>icon('themeicon',t==='dark'?'i-sun':'i-moon'))})();
function showAdmin(){T('view-dash').style.display='none';T('view-admin').style.display='block';scrollTo(0,0)}
function showDash(){T('view-admin').style.display='none';T('view-dash').style.display='block';scrollTo(0,0)}
function showLogin(){forceLogin=true;T('loginform').style.display='block';T('connectednote').style.display='none'}
function cp(i){navigator.clipboard.writeText(T(i).textContent)}
function post(b){return fetch('/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.json())}
function act(d){post('do='+d).then(()=>setTimeout(load,700)).catch(e=>{})}
function age(a){return a<0?'noch nie':(a==0?'gerade eben':a+' Min. her')}
function statusOf(s){
if(s.error)return['i-alert','err','Es gibt ein Problem'];
if(s.ap)return['i-wifi','warn','Setup &ndash; bitte WLAN einrichten'];
if(!s.has_token)return['i-key','warn','Noch nicht verbunden'];
if(s.soc==null)return['i-clock','warn','Warte auf Fahrzeugdaten'];
if(s.charging)return['i-bolt','ok','Laedt gerade'];
return['i-check','ok','Alles bereit'];}
function load(){fetch('/status').then(r=>r.json()).then(s=>{
var soc=s.soc==null?null:s.soc;
T('soc').innerHTML=soc==null?'&ndash;':soc;
var r=T('ring');r.style.strokeDasharray=RC;r.style.strokeDashoffset=soc==null?RC:RC*(1-Math.max(0,Math.min(100,soc))/100);
T('range').innerHTML=s.range==null?'&ndash;':Math.round(s.range)+' km';
T('age').textContent=age(s.age_min);
T('vinv').innerHTML=s.vin?s.vin:'&ndash;';
T('bolt').style.display=s.charging?'block':'none';
var st=statusOf(s);icon('pillicon',st[0]);T('pill').className='pill '+st[1];T('statustext').innerHTML=st[2];
// Verbindungs-Card smart ein/ausblenden
if(s.has_token&&!forceLogin){T('loginform').style.display='none';T('connectednote').style.display='block';T('connecttitle').textContent='Porsche-Konto'}
else{T('loginform').style.display='block';T('connectednote').style.display='none';T('connecttitle').textContent='Mit Porsche verbinden'}
if(s.ap){T('setuphint').style.display='block';if(!window._apOpened){window._apOpened=true;showAdmin()}}else{T('setuphint').style.display='none'}
var h=location.host||'192.168.4.1';
T('usoc').textContent='http://'+h+'/soc';T('urange').textContent='http://'+h+'/range';
T('dip').textContent=s.ip||'-';T('drssi').textContent=(s.rssi||0)+' dBm';
T('dcode').textContent=s.http||'-';T('dup').textContent=Math.floor((s.uptime||0)/60)+' min';
T('fw').textContent=s.fw||'?';
if(s.update_status&&!T('ustat').textContent)T('ustat').textContent=s.update_status;
if(document.activeElement.tagName!=='INPUT'){if(s.ssid)T('fssid').value=s.ssid;
T('fvin').value=s.vin||'';T('fint').value=s.interval||10;if(s.update_url!==undefined)T('fupd').value=s.update_url||''}
T('log').textContent=(s.log||[]).join('\n');
drawChart(s.hist,s.hist_min||10);
}).catch(e=>{})}
function drawChart(hist,mins){var c=T('chartcard');
if(!hist||hist.length<2){c.style.display='none';return}c.style.display='block';
var W=300,H=100,p=10,n=hist.length;
var xs=i=>n<2?0:i/(n-1)*W, ys=v=>H-p-Math.max(0,Math.min(100,v))/100*(H-2*p);
var line='';hist.forEach((v,i)=>line+=(i?'L':'M')+xs(i).toFixed(1)+' '+ys(v).toFixed(1)+' ');
var area='M'+xs(0).toFixed(1)+' '+H+' ';hist.forEach((v,i)=>area+='L'+xs(i).toFixed(1)+' '+ys(v).toFixed(1)+' ');
area+='L'+xs(n-1).toFixed(1)+' '+H+' Z';
T('chline').setAttribute('d',line.trim());T('charea').setAttribute('d',area);
var hrs=n*mins/60;T('chspan').textContent=hrs>=1?('letzte '+Math.round(hrs)+' Std'):('letzte '+(n*mins)+' Min');}
function applyAccent(c,c2){var r=document.documentElement.style;r.setProperty('--red',c);r.setProperty('--red2',c2);
var a=T('gs1'),b=T('gs2'),d=T('ags1');if(a)a.setAttribute('stop-color',c2);if(b)b.setAttribute('stop-color',c);if(d)d.setAttribute('stop-color',c);
document.querySelectorAll('#swatches .sw').forEach(e=>e.classList.toggle('active',e.dataset.c===c))}
function setAccent(el){localStorage.acc=el.dataset.c;localStorage.acc2=el.dataset.c2;applyAccent(el.dataset.c,el.dataset.c2)}
if(localStorage.acc)applyAccent(localStorage.acc,localStorage.acc2||localStorage.acc);else applyAccent('#d5001c','#ff2038');
function ls(t){var l=T('lstat');l.style.display=t?'flex':'none';T('lstattext').innerHTML=t||''}
function handleLogin(s){
if(s.login_success){T('capbox').style.display='none';forceLogin=false;ls('');T('lstat').style.display='none';load();return}
if(s.need_captcha){T('capbox').style.display='block';if(s.captcha)T('capimg').src=s.captcha;T('capcode').value='';T('capcode').focus()}
ls(s.login_error||'')}
function doLogin(){ls('Anmeldung laeuft &ndash; einen Moment ...');
post(new URLSearchParams({do:'login',email:T('lmail').value,password:T('lpass').value})).then(handleLogin).catch(e=>ls('Anmeldung fehlgeschlagen.'))}
function doCaptcha(){ls('Pruefe Eingabe ...');
post(new URLSearchParams({do:'captcha',code:T('capcode').value})).then(handleLogin).catch(e=>{})}
function checkUpd(){T('ustat').textContent='Pruefe ...';
post('do=checkupdate').then(s=>{T('ustat').textContent=s.update_status||'';T('ubtn').style.display=s.update_available?'inline-flex':'none'}).catch(e=>{})}
function doUpd(){if(!confirm('Firmware jetzt aktualisieren? Das Geraet startet neu.'))return;
T('ustat').textContent='Update laeuft, Geraet startet neu ...';post('do=update').catch(e=>{})}
load();setInterval(load,3000);
</script></body></html>
)HTML";

void handleRoot()  { server.send_P(200, "text/html", PAGE); }
void handleSoc()   { if (curSoc < 0) server.send(503, "text/plain", "no data");
                     else server.send(200, "text/plain", String(curSoc)); }
void handleRange() { if (curRange < 0) server.send(503, "text/plain", "no data");
                     else server.send(200, "text/plain", String(curRange, 0)); }

void handleStatus() {
  JsonDocument d;
  if (curSoc < 0) d["soc"] = nullptr; else d["soc"] = curSoc;
  if (curRange < 0) d["range"] = nullptr; else d["range"] = curRange;
  d["charging"] = curCharging;
  d["age_min"] = lastFetchMs == 0 ? -1 : (int)((millis() - lastFetchMs) / 60000);
  d["error"] = lastError;
  d["vin"] = resolvedVin.length() ? resolvedVin : cfgVin;
  d["ssid"] = apMode ? "" : cfgSsid;
  d["ap"] = apMode;
  d["wifi"] = (WiFi.status() == WL_CONNECTED);
  d["ip"] = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  d["rssi"] = apMode ? 0 : WiFi.RSSI();
  d["has_token"] = cfgRefresh.length() > 0;
  d["token_valid"] = (accessToken.length() > 0) && ((int32_t)(tokenExpiresAtMs - millis()) > 0);
  d["http"] = lastHttpCode;
  d["interval"] = cfgIntervalMin;
  d["uptime"] = (millis() - bootMs) / 1000;
  d["fw"] = FW_VERSION;
  d["update_status"] = updateStatus;
  d["update_available"] = updateAvailable;
  d["update_url"] = cfgUpdateUrl;
  d["hist_min"] = cfgIntervalMin;
  JsonArray h = d["hist"].to<JsonArray>();
  for (int i = 0; i < histCount; i++) h.add(socHist[(histHead - histCount + i + HIST_N) % HIST_N]);
  JsonArray lg = d["log"].to<JsonArray>();
  for (int i = 0; i < LOG_LINES; i++) {
    String line = logBuf[(logHead + i) % LOG_LINES];
    if (line.length()) lg.add(line);
  }
  String out; serializeJson(d, out);
  server.send(200, "application/json", out);
}

void handleAction() {
  String d = server.arg("do");
  if (d == "refresh")          { logMsg("Manuelle Aktualisierung angefordert."); fetchSoc(); }
  else if (d == "login")       { startWebLogin(server.arg("email"), server.arg("password")); }
  else if (d == "captcha")     { doIdentifierAndFinish(server.arg("code")); }
  else if (d == "checkupdate") { checkUpdate(); }
  else if (d == "update")      { doUpdate(); }
  else { server.send(400, "application/json", "{\"ok\":false}"); return; }

  if (loginSuccess) fetchSoc();  // nach erfolgreichem Login gleich SoC holen
  JsonDocument r;
  r["ok"] = true;
  r["login_success"]     = loginSuccess;
  r["need_captcha"]      = loginNeedCaptcha;
  r["captcha"]           = loginCaptcha;
  r["login_error"]       = loginError;
  r["update_status"]     = updateStatus;
  r["update_available"]  = updateAvailable;
  String out; serializeJson(r, out);
  server.send(200, "application/json", out);
}
void handleSave() {
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  String refresh = server.arg("refresh");
  String vin = server.arg("vin");
  uint32_t interval = server.arg("interval").toInt();
  if (interval < 5) interval = interval == 0 ? 10 : 5;   // API-Schutz: min. 5 Min
  if (interval > 1440) interval = 1440;
  String updUrl = server.arg("update_url");
  String updTok = server.arg("update_token");
  prefs.begin("porsche", false);
  if (ssid.length()) prefs.putString("ssid", ssid);
  if (pass.length()) prefs.putString("pass", pass);          // leer = altes behalten
  if (refresh.length()) prefs.putString("refresh", refresh); // leer = altes behalten
  prefs.putString("vin", vin);
  prefs.putUInt("interval", interval >= 1 ? interval : 10);
  prefs.putString("upd_url", updUrl);
  if (updTok.length()) prefs.putString("upd_tok", updTok);   // leer = altes behalten
  prefs.end();
  server.send(200, "text/html; charset=utf-8",
              "<meta charset='utf-8'>Gespeichert. Der ESP32 startet neu ...");
  delay(1200);
  ESP.restart();
}

// ==========================================================================
//  setup / loop
// ==========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  bootMs = millis();
  loadConfig();
  if (!connectWifi()) {
    startAp();
  } else {
    Serial.print("Verbunden. IP: ");
    Serial.println(WiFi.localIP());
  }
  server.on("/", handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/soc", handleSoc);
  server.on("/range", handleRange);
  server.on("/status", handleStatus);
  server.on("/action", HTTP_POST, handleAction);
  server.onNotFound([]() {
    if (apMode) {   // Captive Portal: jede unbekannte Anfrage (auch OS-Checks) -> Konfigseite
      server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
      server.send(302, "text/plain", "");
    } else {
      server.send(404, "text/plain", "Not found");
    }
  });
  server.begin();

  if (!apMode && !cfgRefresh.isEmpty()) fetchSoc();  // sofort einmal holen
}

void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  if (!apMode && !cfgRefresh.isEmpty()) {
    uint32_t intervalMs = cfgIntervalMin * 60000UL;
    if (lastFetchMs == 0 || millis() - lastFetchMs >= intervalMs) {
      // Reconnect, falls WLAN weg
      if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(500); }
      fetchSoc();
      if (lastError.length() && lastFetchMs == 0) lastFetchMs = millis();  // Backoff
    }
  }
  delay(10);
}
