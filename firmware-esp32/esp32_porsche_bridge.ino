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
#include <Preferences.h>
#include <ArduinoJson.h>
#include <map>

// Firmware-Version (fuer den Online-Updater)
static const char* FW_VERSION = "1.1.0";

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
  WiFi.softAP(AP_SSID, AP_PASS);
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
                "?mf=BATTERY_LEVEL&mf=E_RANGE&mf=RANGE&mf=MILEAGE";
  String payload = apiGet(path, code);
  if (code == 401) {                 // Token abgelaufen -> einmal erneuern
    if (!refreshAccessToken()) return;
    payload = apiGet(path, code);
  }
  if (code != 200) { lastError = "SoC-Abruf HTTP " + String(code); logMsg(lastError); return; }
  JsonDocument doc;
  if (deserializeJson(doc, payload)) { lastError = "SoC-JSON-Fehler."; logMsg(lastError); return; }
  int soc = -1; float range = -1;
  for (JsonObject m : doc["measurements"].as<JsonArray>()) {
    const char* key = m["key"];
    if (!key) continue;
    if (strcmp(key, "BATTERY_LEVEL") == 0) {
      if (m["value"]["percent"].is<float>() || m["value"]["percent"].is<int>())
        soc = (int)round(m["value"]["percent"].as<float>());
    } else if (strcmp(key, "E_RANGE") == 0 || strcmp(key, "RANGE") == 0) {
      if (range < 0 && m["value"]["kilometers"].is<float>())
        range = m["value"]["kilometers"].as<float>();
    }
  }
  if (soc < 0) { lastError = "Kein BATTERY_LEVEL erhalten."; logMsg(lastError); return; }
  curSoc = soc; curRange = range; lastError = ""; lastFetchMs = millis();
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
:root{--bg:#0f1720;--card:#1b2733;--fg:#e8eef4;--mut:#9fb0c0;--acc:#3ea6ff;
--ok:#37d67a;--warn:#ffb020;--err:#ff5470;--line:#2a3a49}
*{box-sizing:border-box}body{margin:0;font-family:Segoe UI,system-ui,sans-serif;
background:var(--bg);color:var(--fg)}.wrap{max-width:720px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:6px 0 2px}.sub{color:var(--mut);font-size:13px;margin-bottom:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:16px;margin:12px 0}
.row{display:flex;gap:12px;flex-wrap:wrap}.metric{flex:1;min-width:110px}
.metric .k{color:var(--mut);font-size:12px}.metric .v{font-size:18px;font-weight:600}
.soc{font-size:44px;font-weight:700}.unit{font-size:18px;color:var(--mut)}
.badge{display:inline-block;padding:3px 9px;border-radius:999px;font-size:12px;font-weight:600;margin-right:4px}
.b-ok{background:rgba(55,214,122,.15);color:var(--ok)}
.b-warn{background:rgba(255,176,32,.15);color:var(--warn)}
.b-err{background:rgba(255,84,112,.15);color:var(--err)}.b-mut{background:#25313d;color:var(--mut)}
label{display:block;margin:10px 0 4px;font-size:13px;color:var(--mut)}
input{width:100%;padding:9px;border-radius:8px;border:1px solid var(--line);background:#0e161e;color:var(--fg)}
button{border:0;border-radius:9px;padding:10px 14px;font-weight:600;cursor:pointer;margin-top:6px}
.primary{background:var(--acc);color:#04121f}.ghost{background:#25313d;color:var(--fg)}
code{background:#0e161e;border:1px solid var(--line);border-radius:6px;padding:3px 6px;word-break:break-all}
.log{font-family:Consolas,monospace;font-size:12px;background:#0e161e;border:1px solid var(--line);
border-radius:8px;padding:10px;max-height:180px;overflow:auto;white-space:pre-wrap}
.hint{color:var(--warn);font-size:13px}</style></head><body><div class="wrap">
<h1>openWB &middot; Porsche SoC-Bridge</h1><div class="sub" id="mode">...</div>
<div class="card"><div class="row" style="align-items:center">
<div class="metric"><div class="k">Ladestand</div><div><span class="soc" id="soc">-</span><span class="unit"> %</span></div></div>
<div class="metric"><div class="k">Reichweite</div><div class="v" id="range">-</div></div>
<div class="metric"><div class="k">Aktualisiert</div><div class="v" id="age">-</div></div></div>
<div style="margin-top:10px"><span class="badge" id="state">...</span>
<span class="badge b-mut" id="wifi">WLAN -</span><span class="badge b-mut" id="tok">Token -</span></div>
<div id="err" class="hint" style="margin-top:8px"></div>
<div style="margin-top:12px"><button class="primary" onclick="act('refresh')">Jetzt aktualisieren</button>
<button class="ghost" onclick="load()">Neu laden</button></div></div>
<div class="card"><b>1 &middot; Porsche-Login</b>
<div class="sub" style="margin:4px 0 8px">Direkt hier anmelden (erzeugt den Zugang) - kein PC-Tool noetig.</div>
<label>E-Mail (Porsche ID)</label><input id="lmail">
<label>Passwort</label><input id="lpass" type="password">
<button class="primary" onclick="doLogin()">Anmelden</button>
<div id="capbox" style="display:none;margin-top:10px">
<div class="sub">Captcha ablesen und eingeben:</div>
<img id="capimg" style="max-width:240px;background:#fff;border-radius:6px;margin:6px 0;display:block">
<input id="capcode" placeholder="Captcha-Code" style="max-width:220px">
<button class="primary" onclick="doCaptcha()">Absenden</button></div>
<div id="lstat" class="hint" style="margin-top:8px"></div></div>
<div class="card"><b>In openWB eintragen</b> &middot; Fahrzeug &rarr; SoC-Modul "HTTP"
<div style="margin-top:8px">SoC-URL: <code id="usoc"></code> <button class="ghost" onclick="cp('usoc')">Kopieren</button></div>
<div style="margin-top:6px">Range-URL: <code id="urange"></code> <button class="ghost" onclick="cp('urange')">Kopieren</button></div></div>
<div class="card"><b>Einrichtung</b><form method="POST" action="/save">
<label>WLAN-Name (SSID)</label><input name="ssid" id="fssid">
<label>WLAN-Passwort</label><input name="pass" type="password" placeholder="(leer = unveraendert)">
<label>Porsche Refresh-Token (aus dem PC-Tool)</label><input name="refresh" placeholder="hier einfuegen zum Aendern">
<label>VIN (optional)</label><input name="vin" id="fvin">
<label>Aktualisierung (Minuten)</label><input name="interval" id="fint">
<label>Update-URL (version.json, optional)</label><input name="update_url" id="fupd">
<label>Update-Token (nur privates Repo, optional)</label><input name="update_token" type="password" placeholder="(leer = unveraendert)">
<button class="primary" type="submit">Speichern &amp; neu starten</button></form>
<div class="sub" style="margin-top:8px">Refresh-Token holen: PC-Tool &rarr; Tab 1 einloggen &rarr; "Refresh-Token kopieren".</div></div>
<div class="card"><b>Firmware-Update</b>
<div class="sub" style="margin:4px 0 8px">Installiert: v<span id="fw">?</span>. Update aus dem Git-Repo.</div>
<div id="ustat" class="sub"></div>
<button class="ghost" onclick="checkUpd()">Auf Updates pruefen</button>
<button class="primary" id="ubtn" style="display:none" onclick="doUpd()">Update installieren</button></div>
<div class="card"><b>Diagnose / Log</b><div class="row" style="margin:8px 0">
<div class="metric"><div class="k">IP</div><div class="v" id="dip">-</div></div>
<div class="metric"><div class="k">Signal</div><div class="v" id="drssi">-</div></div>
<div class="metric"><div class="k">Letzter HTTP-Code</div><div class="v" id="dcode">-</div></div>
<div class="metric"><div class="k">Laufzeit</div><div class="v" id="dup">-</div></div></div>
<div class="log" id="log">...</div></div>
<div class="sub">Inoffizielles Tool &middot; Porsche-Connect-Abo noetig &middot; TLS ohne Zertifikatspruefung.</div>
</div><script>
function cp(i){navigator.clipboard.writeText(document.getElementById(i).textContent)}
function act(d){fetch('/action?do='+d,{method:'POST'}).then(()=>setTimeout(load,700))}
function age(a){return a<0?'noch nie':(a==0?'gerade eben':a+' Min. her')}
function load(){fetch('/status').then(r=>r.json()).then(s=>{
document.getElementById('soc').textContent=s.soc==null?'-':s.soc;
document.getElementById('range').textContent=s.range==null?'-':Math.round(s.range)+' km';
document.getElementById('age').textContent=age(s.age_min);
var st=document.getElementById('state');
if(s.error){st.className='badge b-err';st.textContent='Fehler'}
else if(s.soc==null){st.className='badge b-warn';st.textContent='wartet auf Daten'}
else{st.className='badge b-ok';st.textContent='OK'}
document.getElementById('err').textContent=s.error||'';
var w=document.getElementById('wifi');w.textContent='WLAN '+(s.ssid||'-');w.className='badge '+(s.wifi?'b-ok':'b-warn');
var t=document.getElementById('tok');t.textContent=s.has_token?(s.token_valid?'Token gueltig':'Token gespeichert'):'kein Token';
t.className='badge '+(s.has_token?(s.token_valid?'b-ok':'b-warn'):'b-err');
document.getElementById('mode').textContent=s.ap?'Setup-Modus (AP): bitte WLAN + Token eintragen.':'Verbunden mit '+(s.ssid||'?');
var h=location.host;document.getElementById('usoc').textContent='http://'+h+'/soc';
document.getElementById('urange').textContent='http://'+h+'/range';
document.getElementById('dip').textContent=s.ip||'-';
document.getElementById('drssi').textContent=(s.rssi||0)+' dBm';
document.getElementById('dcode').textContent=s.http||'-';
document.getElementById('dup').textContent=Math.floor((s.uptime||0)/60)+' min';
document.getElementById('fw').textContent=s.fw||'?';
if(s.update_status&&!document.getElementById('ustat').textContent)document.getElementById('ustat').textContent=s.update_status;
if(document.activeElement.tagName!=='INPUT'){if(s.ssid)document.getElementById('fssid').value=s.ssid;
document.getElementById('fvin').value=s.vin||'';document.getElementById('fint').value=s.interval||10;
if(s.update_url!==undefined)document.getElementById('fupd').value=s.update_url||'';}
document.getElementById('log').textContent=(s.log||[]).join('\n');
}).catch(e=>{})}
function post(b){return fetch('/action',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:b}).then(r=>r.json())}
function handleLogin(s){
if(s.login_success){document.getElementById('capbox').style.display='none';
document.getElementById('lstat').textContent='Login erfolgreich! Zugang gespeichert.';load();return;}
if(s.need_captcha){document.getElementById('capbox').style.display='block';
if(s.captcha)document.getElementById('capimg').src=s.captcha;
document.getElementById('capcode').value='';document.getElementById('capcode').focus();}
document.getElementById('lstat').textContent=s.login_error||''}
function doLogin(){document.getElementById('lstat').textContent='Anmeldung laeuft (kann etwas dauern) ...';
post(new URLSearchParams({do:'login',email:document.getElementById('lmail').value,
password:document.getElementById('lpass').value})).then(handleLogin).catch(e=>{
document.getElementById('lstat').textContent='Fehler bei der Anmeldung.'})}
function doCaptcha(){document.getElementById('lstat').textContent='Pruefe Captcha ...';
post(new URLSearchParams({do:'captcha',code:document.getElementById('capcode').value})).then(handleLogin).catch(e=>{})}
function checkUpd(){document.getElementById('ustat').textContent='Pruefe ...';
post('do=checkupdate').then(s=>{document.getElementById('ustat').textContent=s.update_status||'';
document.getElementById('ubtn').style.display=s.update_available?'inline-block':'none'}).catch(e=>{})}
function doUpd(){if(!confirm('Firmware jetzt aktualisieren? Das Geraet startet neu.'))return;
document.getElementById('ustat').textContent='Update laeuft, Geraet startet neu ...';
post('do=update').catch(e=>{})}
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
  server.begin();

  if (!apMode && !cfgRefresh.isEmpty()) fetchSoc();  // sofort einmal holen
}

void loop() {
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
