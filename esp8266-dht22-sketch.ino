#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <DHT.h>
#include <ArduinoJson.h>

#define DHT_PIN D2
#define RELAY_PIN D6
#define BOOT_BUTTON_PIN 0
#define DHT_TYPE DHT22
#define EEPROM_SIZE 512
#define CONFIG_MAGIC 0xDEADBEEF
#define CONFIG_VERSION 1
#define RELAY_ACTIVE_LOW true
#define WIFI_SSID_MAX 32
#define WIFI_PASS_MAX 64

const IPAddress AP_IP(192, 168, 4, 1);
const IPAddress AP_GATEWAY(192, 168, 4, 1);
const IPAddress AP_SUBNET(255, 255, 255, 0);
const byte DNS_PORT = 53;

DHT dht(DHT_PIN, DHT_TYPE);
ESP8266WebServer server(80);
DNSServer dnsServer;

struct Config {
  uint32_t magic;
  uint16_t version;
  uint16_t crc;
  char ssid[WIFI_SSID_MAX + 1];
  char password[WIFI_PASS_MAX + 1];
  float tempOn;
  float tempOff;
  float humOn;
  float humOff;
  uint8_t relayMode;
  uint8_t relayState;
  uint8_t relayManual;
  uint8_t pad[8];
} __attribute__((packed));

Config cfg;
Config pendingCfg;
bool pendingSave = false;
uint32_t pendingSaveAt = 0;
const uint32_t SAVE_DEBOUNCE_MS = 15000;

volatile bool g_factoryResetRequest = false;

float g_temp = NAN;
float g_hum = NAN;
bool g_dhtValid = false;
uint32_t g_dhtLastRead = 0;
uint32_t g_lastGoodRead = 0;
uint32_t g_dhtBackoff = 2000;
const uint32_t DHT_MIN_INTERVAL = 2000;
const uint32_t DHT_MAX_BACKOFF = 60000;
const uint32_t DHT_STALE_MS = 30000;

bool g_relayState = false;

uint32_t g_bootPressStart = 0;
bool g_bootPressed = false;
const uint32_t BOOT_HOLD_MS = 5000;

uint32_t g_uptimeBase = 0;
uint16_t g_crcErrors = 0;

uint32_t g_requestCount = 0;
uint32_t g_nvsWrites = 0;

String g_jsonBuf;
static String g_pageShell;

uint16_t crcByte(uint16_t crc, uint8_t b) {
  crc ^= b;
  for (uint8_t i = 0; i < 8; i++) {
    if (crc & 1) crc = (uint16_t)((crc >> 1) ^ 0xA001);
    else crc >>= 1;
  }
  return crc;
}

uint16_t calcConfigCRC(const Config* c) {
  uint16_t crc = 0xFFFF;
  const uint8_t* p = (const uint8_t*)c;
  size_t skipLo = offsetof(Config, crc);
  size_t skipHi = skipLo + sizeof(uint16_t);
  for (size_t i = 0; i < sizeof(Config); i++) {
    if (i >= skipLo && i < skipHi) continue;
    crc = crcByte(crc, p[i]);
  }
  return crc;
}

void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  cfg.magic = CONFIG_MAGIC;
  cfg.version = CONFIG_VERSION;
  strncpy(cfg.ssid, "ESP8266_DHT22", WIFI_SSID_MAX);
  strncpy(cfg.password, "12345678", WIFI_PASS_MAX);
  cfg.tempOn = 30.0f;
  cfg.tempOff = 28.0f;
  cfg.humOn = 70.0f;
  cfg.humOff = 65.0f;
  cfg.relayMode = 1;
  cfg.relayState = 0;
  cfg.relayManual = 0;
  cfg.crc = calcConfigCRC(&cfg);
}

bool loadConfig() {
  Config tmp;
  EEPROM.get(0, tmp);
  if (tmp.magic != CONFIG_MAGIC || tmp.version != CONFIG_VERSION) {
    return false;
  }
  if (calcConfigCRC(&tmp) != tmp.crc) {
    g_crcErrors++;
    return false;
  }
  memcpy(&cfg, &tmp, sizeof(cfg));
  return true;
}

void saveConfigNow() {
  cfg.crc = calcConfigCRC(&cfg);
  EEPROM.put(0, cfg);
  if (EEPROM.commit()) g_nvsWrites++;
  pendingSave = false;
  memcpy(&pendingCfg, &cfg, sizeof(cfg));
}

void scheduleSave() {
  memcpy(&pendingCfg, &cfg, sizeof(cfg));
  pendingSave = true;
  pendingSaveAt = millis() + SAVE_DEBOUNCE_MS;
}

void applyRelay(bool state) {
  if (state == g_relayState) return;
  g_relayState = state;
  cfg.relayState = state ? 1 : 0;
  bool level = RELAY_ACTIVE_LOW ? !state : state;
  digitalWrite(RELAY_PIN, level ? HIGH : LOW);
}

void readDHTSafe() {
  uint32_t now = millis();
  if (now - g_dhtLastRead < g_dhtBackoff) return;
  g_dhtLastRead = now;
  float t = dht.readTemperature();
  float h = dht.readHumidity();
  if (isnan(t) || isnan(h) || t < -40.0f || t > 80.0f || h < 0.0f || h > 100.0f) {
    if (g_dhtBackoff < DHT_MAX_BACKOFF) {
      uint32_t nb = g_dhtBackoff * 2;
      g_dhtBackoff = (nb > DHT_MAX_BACKOFF) ? DHT_MAX_BACKOFF : nb;
    }
    if (g_lastGoodRead != 0 && (now - g_lastGoodRead) > DHT_STALE_MS) {
      g_dhtValid = false;
    }
    return;
  }
  g_temp = t;
  g_hum = h;
  g_dhtValid = true;
  g_lastGoodRead = now;
  g_dhtBackoff = DHT_MIN_INTERVAL;
}

void evaluateRelay() {
  if (cfg.relayManual) {
    applyRelay(cfg.relayState == 1);
    return;
  }
  if (cfg.relayMode == 0) {
    applyRelay(false);
    return;
  }
  if (!g_dhtValid) return;
  bool state = g_relayState;
  if (cfg.relayMode == 1) {
    if (g_temp >= cfg.tempOn) state = true;
    else if (g_temp <= cfg.tempOff) state = false;
  } else if (cfg.relayMode == 2) {
    if (g_hum >= cfg.humOn) state = true;
    else if (g_hum <= cfg.humOff) state = false;
  } else if (cfg.relayMode == 3) {
    if (g_temp >= cfg.tempOn || g_hum >= cfg.humOn) state = true;
    else if (g_temp <= cfg.tempOff && g_hum <= cfg.humOff) state = false;
  }
  applyRelay(state);
}

void handleBootButton() {
  bool pressed = digitalRead(BOOT_BUTTON_PIN) == LOW;
  uint32_t now = millis();
  if (pressed) {
    if (!g_bootPressed) {
      g_bootPressed = true;
      g_bootPressStart = now;
    } else if (now - g_bootPressStart >= BOOT_HOLD_MS) {
      g_factoryResetRequest = true;
      g_bootPressed = false;
    }
  } else {
    g_bootPressed = false;
  }
}

void performFactoryReset() {
  setDefaults();
  saveConfigNow();
  delay(100);
  ESP.restart();
}

void startWiFiAP() {
  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(cfg.ssid, cfg.password);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_IP);
}

String escapeJson(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04X", c); out += b; }
    else out += c;
  }
  return out;
}

void addCommonHeaders() {
  server.sendHeader("Pragma", "no-cache");
  server.sendHeader("Connection", "close");
  server.sendHeader("Access-Control-Allow-Origin", "*");
}

void sendJSON(int code, const String& body) {
  g_requestCount++;
  addCommonHeaders();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(code, "application/json", body);
}

void sendHTML(const String& body) {
  g_requestCount++;
  addCommonHeaders();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.send(200, "text/html; charset=utf-8", body);
}

void sendCSS() {
  g_requestCount++;
  addCommonHeaders();
  server.sendHeader("Cache-Control", "max-age=86400");
  static const char CSS_CONTENT[] PROGMEM = R"CSS(
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,system-ui,Segoe UI,Roboto,sans-serif;background:#0f172a;color:#e2e8f0;min-height:100vh}
nav{background:#1e293b;padding:12px 16px;display:flex;flex-wrap:wrap;gap:8px;border-bottom:1px solid #334155;position:sticky;top:0;z-index:100}
nav a{color:#94a3b8;text-decoration:none;padding:8px 14px;border-radius:8px;font-size:14px;transition:.2s;white-space:nowrap}
nav a:hover{background:#334155;color:#e2e8f0}
nav a.active{background:#3b82f6;color:#fff}
.container{max-width:1100px;margin:0 auto;padding:16px}
h1{font-size:22px;margin-bottom:16px;color:#f1f5f9}
h2{font-size:17px;margin-bottom:10px;color:#cbd5e1}
.card{background:#1e293b;border-radius:12px;padding:18px;margin-bottom:16px;border:1px solid #334155}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:12px}
.metric{background:#0f172a;border-radius:10px;padding:14px;text-align:center;border:1px solid #334155}
.metric .v{font-size:26px;font-weight:700;color:#3b82f6}
.metric .l{font-size:12px;color:#94a3b8;margin-top:4px;text-transform:uppercase;letter-spacing:.5px}
.metric.on .v{color:#22c55e}
.metric.off .v{color:#64748b}
.metric.err .v{color:#ef4444}
label{display:block;font-size:13px;color:#94a3b8;margin-bottom:5px;margin-top:10px}
input,select{width:100%;padding:10px 12px;border-radius:8px;border:1px solid #334155;background:#0f172a;color:#e2e8f0;font-size:14px;outline:none;transition:.2s}
input:focus,select:focus{border-color:#3b82f6;box-shadow:0 0 0 3px rgba(59,130,246,.15)}
button{padding:10px 20px;border-radius:8px;border:none;background:#3b82f6;color:#fff;font-size:14px;font-weight:600;cursor:pointer;transition:.2s;margin-top:12px}
button:hover{background:#2563eb}
button.danger{background:#ef4444}button.danger:hover{background:#dc2626}
button.success{background:#22c55e}button.success:hover{background:#16a34a}
button.ghost{background:#334155}button.ghost:hover{background:#475569}
.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
.toast{position:fixed;bottom:20px;left:50%;transform:translateX(-50%) translateY(100px);background:#22c55e;color:#fff;padding:12px 24px;border-radius:10px;font-weight:600;transition:.3s;z-index:999;opacity:0}
.toast.show{transform:translateX(-50%) translateY(0);opacity:1}
.toast.err{background:#ef4444}
.status-dot{display:inline-block;width:10px;height:10px;border-radius:50%;margin-right:6px}
.dot-on{background:#22c55e;box-shadow:0 0 8px #22c55e}
.dot-off{background:#64748b}
.badge{padding:3px 10px;border-radius:20px;font-size:11px;font-weight:700;text-transform:uppercase;letter-spacing:.5px}
.badge.ok{background:#14532d;color:#86efac}
.badge.warn{background:#713f12;color:#fde047}
.badge.err{background:#7f1d1d;color:#fca5a5}
table{width:100%;border-collapse:collapse;font-size:13px}
td,th{padding:10px 8px;text-align:left;border-bottom:1px solid #334155}
th{color:#94a3b8;font-weight:600;font-size:12px;text-transform:uppercase;letter-spacing:.5px}
.footer{text-align:center;padding:20px;color:#475569;font-size:12px}
@media(max-width:600px){nav{padding:8px}.container{padding:12px}.card{padding:14px}h1{font-size:18px}.metric .v{font-size:22px}}
)CSS";
  server.send_P(200, PSTR("text/css"), CSS_CONTENT);
}

String navBar(const String& active) {
  auto cls = [&](const char* name) { return active == name ? "class='active'" : ""; };
  String n;
  n.reserve(384);
  n += "<nav>";
  n += "<a href='/' " + String(cls("dash")) + ">Dashboard</a>";
  n += "<a href='/sensors' " + String(cls("sensors")) + ">Sensors</a>";
  n += "<a href='/wifi' " + String(cls("wifi")) + ">WiFi AP</a>";
  n += "<a href='/status' " + String(cls("status")) + ">Status</a>";
  n += "</nav>";
  return n;
}

String pageShell(const String& title, const String& active, const String& body) {
  g_pageShell = "";
  size_t needed = body.length() + title.length() + 1200;
  g_pageShell.reserve(needed);
  g_pageShell += F("<!DOCTYPE html><html lang='en'><head><meta charset='utf-8'><meta name='viewport' content='width=device-width,initial-scale=1'>");
  g_pageShell += "<title>";
  g_pageShell += title;
  g_pageShell += "</title><link rel='stylesheet' href='/style.css'></head><body>";
  g_pageShell += navBar(active);
  g_pageShell += "<div class='container' id='app'>";
  g_pageShell += body;
  g_pageShell += "</div><div class='footer'>ESP8266 DHT22 Controller</div><div class='toast' id='toast'></div>";
  g_pageShell += F("<script>window.__api=function(u,o){return fetch(u,o).then(function(r){return r.json()})};</script>");
  g_pageShell += "</body></html>";
  return g_pageShell;
}

void handleIndex() {
  String body;
  body.reserve(2400);
  body += F("<h1>Live Dashboard</h1>");
  body += F("<div class='card'><div class='grid'>");
  body += F("<div class='metric' id='mTemp'><div class='v' id='tempVal'>--</div><div class='l'>Temperature &deg;C</div></div>");
  body += F("<div class='metric' id='mHum'><div class='v' id='humVal'>--</div><div class='l'>Humidity %</div></div>");
  body += F("<div class='metric off' id='mRelay'><div class='v' id='relayVal'>OFF</div><div class='l'>Relay (D6)</div></div>");
  body += F("<div class='metric' id='mSensor'><div class='v' id='sensorVal'>--</div><div class='l'>Sensor</div></div>");
  body += F("</div></div>");
  body += F("<div class='card'><h2>Controls</h2><div class='row'>");
  body += F("<button class='success' onclick=\"ctl('/api/relay',{state:true})\">Relay ON</button>");
  body += F("<button class='ghost' onclick=\"ctl('/api/relay',{state:false})\">Relay OFF</button>");
  body += F("<button onclick=\"ctl('/api/relay',{auto:true})\">Auto Mode</button>");
  body += F("</div><div style='margin-top:12px' id='modeText'>Mode: --</div></div>");
  body += F("<div class='card'><h2>Live Readings</h2><table><tr><th>Metric</th><th>Value</th></tr>");
  body += F("<tr><td>Temperature</td><td id='t2'>--</td></tr><tr><td>Humidity</td><td id='h2'>--</td></tr>");
  body += F("<tr><td>Relay State</td><td id='r2'>--</td></tr><tr><td>Uptime</td><td id='up2'>--</td></tr></table></div>");
  body += F("<script>");
  body += F("function toast(m,e){var t=document.getElementById('toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(function(){t.className='toast'},2500)}");
  body += F("function ctl(u,b){window.__api(u,{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(function(){toast('OK')}).catch(function(){toast('Failed',1)})}");
  body += F("function refresh(){window.__api('/api/status').then(function(d){");
  body += F("document.getElementById('tempVal').textContent=d.valid?d.temp.toFixed(1):'--'");
  body += F("document.getElementById('humVal').textContent=d.valid?d.hum.toFixed(1):'--'");
  body += F("document.getElementById('t2').textContent=d.valid?d.temp.toFixed(2)+' C':'N/A'");
  body += F("document.getElementById('h2').textContent=d.valid?d.hum.toFixed(2)+' %':'N/A'");
  body += F("var r=document.getElementById('relayVal');r.textContent=d.relay?'ON':'OFF';");
  body += F("document.getElementById('mRelay').className='metric '+(d.relay?'on':'off')");
  body += F("document.getElementById('r2').textContent=d.relay?'ON':'OFF'");
  body += F("var s=document.getElementById('sensorVal');s.textContent=d.valid?'OK':'ERR';");
  body += F("document.getElementById('mSensor').className='metric '+(d.valid?'on':'err')");
  body += F("document.getElementById('up2').textContent=Math.floor(d.uptime/1000)+'s'");
  body += F("document.getElementById('modeText').textContent='Mode: '+(d.manual?'Manual':'Auto')+' | Threshold Temp '+d.tempOn+'/'+d.tempOff+' Hum '+d.humOn+'/'+d.humOff");
  body += F("}).catch(function(){})}");
  body += F("refresh();setInterval(refresh,3000);");
  body += F("</script>");
  sendHTML(pageShell("Dashboard", "dash", body));
}

void handleSensorsPage() {
  String body;
  body.reserve(2400);
  body += F("<h1>Sensor Configuration</h1>");
  body += F("<div class='card'><h2>Thresholds</h2>");
  body += F("<label>Relay Mode</label><select id='mode'>");
  body += F("<option value='0'>Off</option><option value='1'>Temperature</option><option value='2'>Humidity</option><option value='3'>Temp + Hum</option></select>");
  body += F("<div class='grid' style='margin-top:12px'>");
  body += F("<div><label>Temp ON &deg;C</label><input type='number' step='0.1' id='tempOn'></div>");
  body += F("<div><label>Temp OFF &deg;C</label><input type='number' step='0.1' id='tempOff'></div>");
  body += F("<div><label>Hum ON %</label><input type='number' step='0.1' id='humOn'></div>");
  body += F("<div><label>Hum OFF %</label><input type='number' step='0.1' id='humOff'></div>");
  body += F("</div><button onclick='save()'>Save Settings</button></div>");
  body += F("<div class='card'><h2>Manual Relay Override</h2><div class='row'>");
  body += F("<button class='success' onclick=\"man(true)\">Force ON</button>");
  body += F("<button class='ghost' onclick=\"man(false)\">Force OFF</button>");
  body += F("<button onclick='autoMode()'>Return to Auto</button></div></div>");
  body += F("<script>");
  body += F("function toast(m,e){var t=document.getElementById('toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(function(){t.className='toast'},2500)}");
  body += F("function load(){window.__api('/api/config').then(function(d){");
  body += F("document.getElementById('mode').value=d.relayMode;");
  body += F("document.getElementById('tempOn').value=d.tempOn;");
  body += F("document.getElementById('tempOff').value=d.tempOff;");
  body += F("document.getElementById('humOn').value=d.humOn;");
  body += F("document.getElementById('humOff').value=d.humOff;})}");
  body += F("function save(){var b={relayMode:+document.getElementById('mode').value,");
  body += F("tempOn:+document.getElementById('tempOn').value,tempOff:+document.getElementById('tempOff').value,");
  body += F("humOn:+document.getElementById('humOn').value,humOff:+document.getElementById('humOff').value};");
  body += F("window.__api('/api/config',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)}).then(function(r){toast(r.ok?'Saved':'Error',!r.ok)})}");
  body += F("function man(s){window.__api('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({manual:true,state:s})}).then(function(){toast('Manual '+(s?'ON':'OFF'))})}");
  body += F("function autoMode(){window.__api('/api/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({auto:true})}).then(function(){toast('Auto mode')})}");
  body += F("load();");
  body += F("</script>");
  sendHTML(pageShell("Sensors", "sensors", body));
}

void handleWiFiPage() {
  String body;
  body.reserve(1800);
  body += F("<h1>WiFi AP Management</h1>");
  body += F("<div class='card'><h2>Access Point Settings</h2>");
  body += F("<label>SSID</label><input id='ssid' maxlength='32'>");
  body += F("<label>Password (8-63 chars)</label><input id='pass' type='password' maxlength='64'>");
  body += F("<button onclick='saveWifi()'>Apply &amp; Restart AP</button></div>");
  body += F("<div class='card'><h2>Current AP</h2><table>");
  body += F("<tr><th>SSID</th><td id='curSsid'>--</td></tr>");
  body += F("<tr><th>IP</th><td>192.168.4.1</td></tr>");
  body += F("<tr><th>Clients</th><td id='clients'>--</td></tr></table></div>");
  body += F("<script>");
  body += F("function toast(m,e){var t=document.getElementById('toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(function(){t.className='toast'},2500)}");
  body += F("function load(){window.__api('/api/wifi').then(function(d){");
  body += F("document.getElementById('ssid').value=d.ssid;");
  body += F("document.getElementById('curSsid').textContent=d.ssid;");
  body += F("document.getElementById('clients').textContent=d.clients;})}");
  body += F("function saveWifi(){var s=document.getElementById('ssid').value;var p=document.getElementById('pass').value;");
  body += F("if(!s||s.length>32){toast('Invalid SSID',1);return}");
  body += F("if(p.length<8||p.length>63){toast('Password must be 8-63 chars',1);return}");
  body += F("window.__api('/api/wifi',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({ssid:s,password:p})}).then(function(r){toast('Saved, restarting AP...')})}");
  body += F("load();setInterval(load,5000);");
  body += F("</script>");
  sendHTML(pageShell("WiFi AP", "wifi", body));
}

void handleStatusPage() {
  String body;
  body.reserve(2000);
  body += F("<h1>System Status</h1>");
  body += F("<div class='card'><h2>Runtime</h2><div class='grid'>");
  body += F("<div class='metric'><div class='v' id='up'>--</div><div class='l'>Uptime</div></div>");
  body += F("<div class='metric'><div class='v' id='heap'>--</div><div class='l'>Free Heap</div></div>");
  body += F("<div class='metric'><div class='v' id='frag'>--</div><div class='l'>Frag %</div></div>");
  body += F("<div class='metric'><div class='v' id='rssi'>--</div><div class='l'>RSSI</div></div>");
  body += F("</div></div>");
  body += F("<div class='card'><h2>Counters</h2><table>");
  body += F("<tr><th>Requests</th><td id='req'>--</td></tr>");
  body += F("<tr><th>NVS Writes</th><td id='nvs'>--</td></tr>");
  body += F("<tr><th>CRC Errors</th><td id='crc'>--</td></tr>");
  body += F("<tr><th>WiFi Clients</th><td id='wc'>--</td></tr></table></div>");
  body += F("<div class='card'><h2>Actions</h2><div class='row'>");
  body += F("<button class='ghost' onclick=\"act('/api/restart')\">Restart</button>");
  body += F("<button class='danger' onclick=\"if(confirm('Factory reset?'))act('/api/factory')\">Factory Reset</button></div></div>");
  body += F("<script>");
  body += F("function toast(m,e){var t=document.getElementById('toast');t.textContent=m;t.className='toast show'+(e?' err':'');setTimeout(function(){t.className='toast'},2500)}");
  body += F("function act(u){window.__api(u,{method:'POST'}).then(function(){toast('Done')})}");
  body += F("function load(){window.__api('/api/system').then(function(d){");
  body += F("document.getElementById('up').textContent=Math.floor(d.uptime/1000)+'s'");
  body += F("document.getElementById('heap').textContent=Math.floor(d.heap/1024)+'K'");
  body += F("document.getElementById('frag').textContent=d.frag+'%'");
  body += F("document.getElementById('rssi').textContent=(d.rssi===0?'N/A':d.rssi+'dBm')");
  body += F("document.getElementById('req').textContent=d.requests");
  body += F("document.getElementById('nvs').textContent=d.nvsWrites");
  body += F("document.getElementById('crc').textContent=d.crcErrors");
  body += F("document.getElementById('wc').textContent=d.clients;})}");
  body += F("load();setInterval(load,4000);");
  body += F("</script>");
  sendHTML(pageShell("Status", "status", body));
}

void handleApiStatus() {
  g_jsonBuf = "";
  g_jsonBuf.reserve(320);
  g_jsonBuf += "{\"temp\":" + String(g_dhtValid ? g_temp : 0, 2);
  g_jsonBuf += ",\"hum\":" + String(g_dhtValid ? g_hum : 0, 2);
  g_jsonBuf += ",\"valid\":" + String(g_dhtValid ? "true" : "false");
  g_jsonBuf += ",\"relay\":" + String(g_relayState ? "true" : "false");
  g_jsonBuf += ",\"manual\":" + String(cfg.relayManual ? "true" : "false");
  g_jsonBuf += ",\"tempOn\":" + String(cfg.tempOn, 1);
  g_jsonBuf += ",\"tempOff\":" + String(cfg.tempOff, 1);
  g_jsonBuf += ",\"humOn\":" + String(cfg.humOn, 1);
  g_jsonBuf += ",\"humOff\":" + String(cfg.humOff, 1);
  g_jsonBuf += ",\"uptime\":" + String(millis() - g_uptimeBase);
  g_jsonBuf += "}";
  sendJSON(200, g_jsonBuf);
}

void handleApiConfig() {
  if (server.method() == HTTP_GET) {
    g_jsonBuf = "";
    g_jsonBuf.reserve(280);
    g_jsonBuf += "{\"relayMode\":" + String(cfg.relayMode);
    g_jsonBuf += ",\"tempOn\":" + String(cfg.tempOn, 1);
    g_jsonBuf += ",\"tempOff\":" + String(cfg.tempOff, 1);
    g_jsonBuf += ",\"humOn\":" + String(cfg.humOn, 1);
    g_jsonBuf += ",\"humOff\":" + String(cfg.humOff, 1);
    g_jsonBuf += "}";
    sendJSON(200, g_jsonBuf);
    return;
  }
  if (server.hasArg("plain")) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJSON(400, "{\"ok\":false,\"err\":\"json\"}");
      return;
    }
    if (doc.containsKey("relayMode")) {
      int m = doc["relayMode"];
      if (m >= 0 && m <= 3) cfg.relayMode = m;
    }
    if (doc.containsKey("tempOn")) cfg.tempOn = doc["tempOn"];
    if (doc.containsKey("tempOff")) cfg.tempOff = doc["tempOff"];
    if (doc.containsKey("humOn")) cfg.humOn = doc["humOn"];
    if (doc.containsKey("humOff")) cfg.humOff = doc["humOff"];
    if (cfg.tempOn < -40) cfg.tempOn = -40;
    if (cfg.tempOn > 80) cfg.tempOn = 80;
    if (cfg.tempOff < -40) cfg.tempOff = -40;
    if (cfg.tempOff > 80) cfg.tempOff = 80;
    if (cfg.humOn < 0) cfg.humOn = 0;
    if (cfg.humOn > 100) cfg.humOn = 100;
    if (cfg.humOff < 0) cfg.humOff = 0;
    if (cfg.humOff > 100) cfg.humOff = 100;
    if (cfg.tempOn < cfg.tempOff) {
      float x = cfg.tempOn; cfg.tempOn = cfg.tempOff; cfg.tempOff = x;
    }
    if (cfg.humOn < cfg.humOff) {
      float x = cfg.humOn; cfg.humOn = cfg.humOff; cfg.humOff = x;
    }
    scheduleSave();
    sendJSON(200, "{\"ok\":true}");
    return;
  }
  sendJSON(400, "{\"ok\":false}");
}

void handleApiRelay() {
  if (server.method() != HTTP_POST) { sendJSON(405, "{\"ok\":false}"); return; }
  if (server.hasArg("plain")) {
    StaticJsonDocument<128> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJSON(400, "{\"ok\":false,\"err\":\"json\"}");
      return;
    }
    if (doc.containsKey("auto")) {
      cfg.relayManual = 0;
      scheduleSave();
    } else if (doc.containsKey("manual")) {
      cfg.relayManual = 1;
      bool s = doc["state"] | false;
      cfg.relayState = s ? 1 : 0;
      applyRelay(s);
      scheduleSave();
    } else if (doc.containsKey("state")) {
      bool s = doc["state"];
      cfg.relayManual = 1;
      cfg.relayState = s ? 1 : 0;
      applyRelay(s);
      scheduleSave();
    }
    sendJSON(200, "{\"ok\":true}");
    return;
  }
  sendJSON(400, "{\"ok\":false}");
}

void handleApiWifi() {
  if (server.method() == HTTP_GET) {
    g_jsonBuf = "";
    g_jsonBuf.reserve(160);
    g_jsonBuf += "{\"ssid\":\"" + escapeJson(String(cfg.ssid)) + "\"";
    g_jsonBuf += ",\"clients\":" + String(WiFi.softAPgetStationNum());
    g_jsonBuf += "}";
    sendJSON(200, g_jsonBuf);
    return;
  }
  if (server.hasArg("plain")) {
    StaticJsonDocument<256> doc;
    if (deserializeJson(doc, server.arg("plain"))) {
      sendJSON(400, "{\"ok\":false,\"err\":\"json\"}");
      return;
    }
    const char* ssid = doc["ssid"] | "";
    const char* pass = doc["password"] | "";
    size_t sl = strlen(ssid);
    size_t pl = strlen(pass);
    if (sl == 0 || sl > WIFI_SSID_MAX) { sendJSON(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; }
    bool allSpace = true;
    for (size_t i = 0; i < sl; i++) { if (ssid[i] != ' ') { allSpace = false; break; } }
    if (allSpace) { sendJSON(400, "{\"ok\":false,\"err\":\"ssid\"}"); return; }
    if (pl < 8 || pl > WIFI_PASS_MAX) { sendJSON(400, "{\"ok\":false,\"err\":\"pass\"}"); return; }
    memset(cfg.ssid, 0, sizeof(cfg.ssid));
    memset(cfg.password, 0, sizeof(cfg.password));
    strncpy(cfg.ssid, ssid, WIFI_SSID_MAX);
    strncpy(cfg.password, pass, WIFI_PASS_MAX);
    saveConfigNow();
    sendJSON(200, "{\"ok\":true}");
    delay(200);
    WiFi.softAPdisconnect(true);
    delay(100);
    startWiFiAP();
    return;
  }
  sendJSON(400, "{\"ok\":false}");
}

void handleApiSystem() {
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t frag = ESP.getHeapFragmentation();
  g_jsonBuf = "";
  g_jsonBuf.reserve(260);
  g_jsonBuf += "{\"uptime\":" + String(millis() - g_uptimeBase);
  g_jsonBuf += ",\"heap\":" + String(freeHeap);
  g_jsonBuf += ",\"frag\":" + String(frag);
  g_jsonBuf += ",\"rssi\":0";
  g_jsonBuf += ",\"requests\":" + String(g_requestCount);
  g_jsonBuf += ",\"nvsWrites\":" + String(g_nvsWrites);
  g_jsonBuf += ",\"crcErrors\":" + String(g_crcErrors);
  g_jsonBuf += ",\"clients\":" + String(WiFi.softAPgetStationNum());
  g_jsonBuf += "}";
  sendJSON(200, g_jsonBuf);
}

void handleApiRestart() {
  sendJSON(200, "{\"ok\":true}");
  delay(100);
  ESP.restart();
}

void handleApiFactory() {
  sendJSON(200, "{\"ok\":true}");
  delay(100);
  performFactoryReset();
}

void handleCaptivePortal() {
  g_requestCount++;
  addCommonHeaders();
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Location", "http://192.168.4.1/", true);
  server.send(302, "text/plain", "");
}

void handleNotFound() {
  if (server.method() == HTTP_GET) {
    handleCaptivePortal();
  } else {
    sendJSON(404, "{\"ok\":false}");
  }
}

void setupRoutes() {
  server.on("/", HTTP_GET, handleIndex);
  server.on("/sensors", HTTP_GET, handleSensorsPage);
  server.on("/wifi", HTTP_GET, handleWiFiPage);
  server.on("/status", HTTP_GET, handleStatusPage);
  server.on("/style.css", HTTP_GET, sendCSS);
  server.on("/api/status", HTTP_GET, handleApiStatus);
  server.on("/api/config", HTTP_GET, handleApiConfig);
  server.on("/api/config", HTTP_POST, handleApiConfig);
  server.on("/api/relay", HTTP_POST, handleApiRelay);
  server.on("/api/wifi", HTTP_GET, handleApiWifi);
  server.on("/api/wifi", HTTP_POST, handleApiWifi);
  server.on("/api/system", HTTP_GET, handleApiSystem);
  server.on("/api/restart", HTTP_POST, handleApiRestart);
  server.on("/api/factory", HTTP_POST, handleApiFactory);
  server.on("/generate_204", HTTP_GET, handleCaptivePortal);
  server.on("/hotspot-detect.html", HTTP_GET, handleCaptivePortal);
  server.on("/connecttest.txt", HTTP_GET, handleCaptivePortal);
  server.on("/ncsi.txt", HTTP_GET, handleCaptivePortal);
  server.on("/fwlink", HTTP_GET, handleCaptivePortal);
  server.onNotFound(handleNotFound);
}

void setup() {
  g_uptimeBase = millis();

  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);

  EEPROM.begin(EEPROM_SIZE);
  if (!loadConfig()) {
    setDefaults();
    saveConfigNow();
  }

  g_relayState = cfg.relayState == 1;
  digitalWrite(RELAY_PIN, (RELAY_ACTIVE_LOW ? !g_relayState : g_relayState) ? HIGH : LOW);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  WiFi.softAPConfig(AP_IP, AP_GATEWAY, AP_SUBNET);
  WiFi.softAP(cfg.ssid, cfg.password);
  dnsServer.setErrorReplyCode(DNSReplyCode::NoError);
  dnsServer.start(DNS_PORT, "*", AP_IP);

  dht.begin();
  g_dhtLastRead = millis();

  setupRoutes();
  server.begin();
}

void loop() {
  dnsServer.processNextRequest();
  server.handleClient();

  handleBootButton();

  if (g_factoryResetRequest) {
    g_factoryResetRequest = false;
    performFactoryReset();
  }

  readDHTSafe();

  evaluateRelay();

  if (pendingSave && (int32_t)(millis() - pendingSaveAt) >= 0) {
    if (memcmp(&pendingCfg, &cfg, sizeof(cfg)) != 0) {
      saveConfigNow();
    } else {
      pendingSave = false;
    }
  }
}
