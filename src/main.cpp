#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Wire.h>
#include <Adafruit_SHT4x.h>
#include "secrets.h"

const char* HOSTNAME = "drivhus";

const unsigned long WIFI_TIMEOUT_MS   = 20000;
const unsigned long WIFI_TJEK_MS      = 30000;
const unsigned long MAALE_INTERVAL_MS = 5000;
const unsigned long SENSOR_RETRY_MS   = 30000;

const int SDA_INDE = 21;
const int SCL_INDE = 22;

WebServer server(80);
Adafruit_SHT4x sht4Inde = Adafruit_SHT4x();

float tempInde = NAN;
float fugtInde = NAN;
float dugInde  = NAN;
bool  sensorIndeOk = false;

unsigned long sidsteMaaling       = 0;
unsigned long sidsteWifiTjek      = 0;
unsigned long sidsteSensorForsoeg = 0;
bool netTjenesterKoerer = false;

const char INDEX_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html><html lang="da"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Drivhus</title>
<style>
  body{font-family:system-ui,sans-serif;background:#111;color:#eee;
       margin:0;padding:2rem;display:flex;gap:1rem;flex-wrap:wrap}
  .kort{background:#1c1c1c;border-radius:12px;padding:1.5rem;min-width:160px}
  .navn{font-size:.8rem;color:#888;text-transform:uppercase;letter-spacing:.05em}
  .vaerdi{font-size:2.5rem;font-weight:600;margin-top:.3rem}
  .enhed{font-size:1rem;color:#888;margin-left:.2rem}
  .lille{font-size:.75rem;color:#666;margin-top:1.5rem;width:100%}
  .fejl{color:#c66}
</style></head><body>
  <div class="kort"><div class="navn">Temperatur inde</div>
    <div class="vaerdi"><span id="t">.</span><span class="enhed">&deg;C</span></div></div>
  <div class="kort"><div class="navn">Luftfugtighed inde</div>
    <div class="vaerdi"><span id="h">.</span><span class="enhed">%</span></div></div>
  <div class="kort"><div class="navn">Dugpunkt inde</div>
    <div class="vaerdi"><span id="d">.</span><span class="enhed">&deg;C</span></div></div>
  <div class="kort"><div class="navn">Oppetid</div>
    <div class="vaerdi"><span id="u">.</span><span class="enhed">min</span></div></div>
  <div class="lille">Signal: <span id="r">.</span> dBm
    <span id="status"></span></div>
<script>
function vis(id, v, decimaler){
  document.getElementById(id).textContent =
    (v === null || v === undefined) ? '--' : v.toFixed(decimaler);
}
async function hent(){
  try{
    const r = await fetch('/api/data');
    const d = await r.json();
    vis('t', d.temp, 1);
    vis('h', d.fugt, 1);
    vis('d', d.dug, 1);
    document.getElementById('u').textContent = Math.floor(d.uptime/60000);
    document.getElementById('r').textContent = d.rssi;
    document.getElementById('status').innerHTML =
      d.sensor_inde ? '' : ' <span class="fejl">sensor inde svarer ikke</span>';
  }catch(e){ console.error(e); }
}
hent(); setInterval(hent, 5000);
</script></body></html>
)rawliteral";

float beregnDugpunkt(float t, float rh) {
  if (isnan(t) || isnan(rh) || rh <= 0) return NAN;
  const float a = 17.62, b = 243.12;
  float gamma = (a * t) / (b + t) + log(rh / 100.0);
  return (b * gamma) / (a - gamma);
}

void talTilJson(char* ud, size_t n, float v) {
  if (isnan(v)) snprintf(ud, n, "null");
  else          snprintf(ud, n, "%.2f", v);
}

void handleRoot() {
  // PROGMEM er uden effekt paa ESP32, saa almindelig send() er nok
  server.send(200, "text/html", INDEX_HTML);
}

void handleData() {
  char t[16], h[16], d[16], buf[220];
  talTilJson(t, sizeof(t), tempInde);
  talTilJson(h, sizeof(h), fugtInde);
  talTilJson(d, sizeof(d), dugInde);

  snprintf(buf, sizeof(buf),
    "{\"temp\":%s,\"fugt\":%s,\"dug\":%s,"
    "\"sensor_inde\":%s,\"rssi\":%d,\"uptime\":%lu}",
    t, h, d, sensorIndeOk ? "true" : "false",
    (int)WiFi.RSSI(), (unsigned long)millis());

  server.send(200, "application/json", buf);
}

bool startSensorInde() {
  if (!sht4Inde.begin(&Wire)) {
    Serial.println("SHT41 inde: ikke fundet paa 0x44");
    return false;
  }
  sht4Inde.setPrecision(SHT4X_HIGH_PRECISION);
  sht4Inde.setHeater(SHT4X_NO_HEATER);
  Serial.printf("SHT41 inde OK, serienummer 0x%08lX\n",
                (unsigned long)sht4Inde.readSerial());
  return true;
}

void laesSensorer() {
  if (!sensorIndeOk) return;

  sensors_event_t fugt, temp;
  if (!sht4Inde.getEvent(&fugt, &temp)) {
    Serial.println("SHT41 inde: aflaesning fejlede");
    sensorIndeOk = false;
    tempInde = fugtInde = dugInde = NAN;
    return;
  }

  tempInde = temp.temperature;
  fugtInde = fugt.relative_humidity;
  dugInde  = beregnDugpunkt(tempInde, fugtInde);

  Serial.printf("Inde: %.2f C  %.2f %%RH  dug %.2f C\n",
                tempInde, fugtInde, dugInde);
}

void startNetTjenester() {
  if (netTjenesterKoerer) return;

  MDNS.begin(HOSTNAME);
  MDNS.addService("http", "tcp", 80);

  ArduinoOTA.setHostname(HOSTNAME);
  ArduinoOTA.setPassword(OTA_PASS);

  ArduinoOTA.onStart([]() {
    Serial.println("OTA: start");
    server.stop();
  });
  ArduinoOTA.onProgress([](unsigned int fremgang, unsigned int total) {
    Serial.printf("OTA: %u%%\r", (fremgang * 100) / total);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nOTA: faerdig, genstarter");
  });
  ArduinoOTA.onError([](ota_error_t fejl) {
    Serial.printf("OTA fejl [%u]: ", (unsigned)fejl);
    if      (fejl == OTA_AUTH_ERROR)    Serial.println("forkert kode");
    else if (fejl == OTA_BEGIN_ERROR)   Serial.println("begin fejlede");
    else if (fejl == OTA_CONNECT_ERROR) Serial.println("forbindelse fejlede");
    else if (fejl == OTA_RECEIVE_ERROR) Serial.println("modtagelse fejlede");
    else if (fejl == OTA_END_ERROR)     Serial.println("afslutning fejlede");
  });

  ArduinoOTA.begin();
  netTjenesterKoerer = true;
  Serial.println("Nettjenester startet (mDNS + OTA)");
}

void forbindWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Hostname: "); Serial.println(WiFi.getHostname());
    Serial.print("IP:       "); Serial.println(WiFi.localIP());
    Serial.print("MAC:      "); Serial.println(WiFi.macAddress());
    Serial.printf("RSSI:     %d dBm\n", (int)WiFi.RSSI());
    startNetTjenester();
  } else {
    Serial.println("WiFi fejlede, koerer videre lokalt");
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println();
  Serial.println("Drivhus starter op");

  Wire.begin(SDA_INDE, SCL_INDE);
  sensorIndeOk = startSensorInde();
  if (sensorIndeOk) laesSensorer();

  forbindWifi();

  server.on("/", handleRoot);
  server.on("/api/data", handleData);
  server.onNotFound([]() { server.send(404, "text/plain", "findes ikke"); });
  server.begin();

  Serial.println("Klar");
}

void loop() {
  ArduinoOTA.handle();
  server.handleClient();

  if (millis() - sidsteWifiTjek > WIFI_TJEK_MS) {
    sidsteWifiTjek = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi vaek, forsoeger igen");
      netTjenesterKoerer = false;
      WiFi.disconnect();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
    } else if (!netTjenesterKoerer) {
      Serial.print("WiFi tilbage, IP: ");
      Serial.println(WiFi.localIP());
      startNetTjenester();
    }
  }

  if (!sensorIndeOk && millis() - sidsteSensorForsoeg > SENSOR_RETRY_MS) {
    sidsteSensorForsoeg = millis();
    Serial.println("Proever sensor inde igen");
    sensorIndeOk = startSensorInde();
  }

  if (millis() - sidsteMaaling > MAALE_INTERVAL_MS) {
    sidsteMaaling = millis();
    laesSensorer();
  }
}