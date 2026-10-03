#include <WiFi.h>
#include <WebServer.h>
#include <esp_now.h>
#include <Preferences.h>

#define MOISTURE_PIN    34
#define ALERT_LED_PIN    2
#define BUZZER_PIN       4

#define AP_SSID     "KrushiSense_Hub"
#define AP_PASS     "krushi1234"
#define AP_CHANNEL  1

// Calibration Limits (Adjust if needed)
const int DRY_ADC = 3200;  // Sukhi mitti / hawa
const int WET_ADC = 1300;  // 100% paani

#define BEEP_MS         400UL
#define EVENT_GAP_MS    10000UL
#define MAX_EVENTS      5

// ESP-NOW Structs
typedef struct struct_message {
  char node[12];
  char label[16];
  float confidence;
} struct_message;

// Command Struct to control Camera Node Mode
typedef struct struct_command {
  bool surveillance_active;
} struct_command;

uint8_t camAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Broadcast

struct Event { char label[16]; float conf; unsigned long ms; };

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
Event events[MAX_EVENTS];
int eventCount = 0;
char lastLabel[16] = "";
float lastConf = 0;
unsigned long lastDetectMs = 0, lastEventMs = 0, totalAlerts = 0;
bool everDetected = false;
volatile bool newAlertFlag = false;

// Time Scheduling (Stored as minutes from midnight: 0 to 1439)
struct Slot {
  bool enabled;
  int startMin;
  int endMin;
};

Slot slots[3];
int currentMinutes = -1; // -1 means unsynced, updated via phone browser
unsigned long lastClockSyncMs = 0;
bool isArmed = false;

unsigned long moistureIntervalMs = 5000;
int moistureThreshold = 30;
unsigned long lastMoistureCheck = 0;
int moisturePct = 0;
bool manualBuzzer = false;
unsigned long beepUntil = 0;

Preferences prefs;
WebServer server(80);

// Helper: Check if current time falls in a slot
bool isTimeInSlot(int cur, int start, int end) {
  if (start <= end) {
    return (cur >= start && cur < end);
  } else {
    // Crosses midnight (e.g., 10 PM to 4 AM)
    return (cur >= start || cur < end);
  }
}

// Non-linear Quadratic Mapping to stop sensitivity spikes
int readMoisturePercentage() {
  long sum = 0;
  for (int i = 0; i < 32; i++) {
    sum += analogRead(MOISTURE_PIN);
    delayMicroseconds(80);
  }
  int raw = sum / 32;
  
  if (raw >= DRY_ADC) return 0;
  if (raw <= WET_ADC) return 100;

  // Normalized fraction 0.0 (Dry) to 1.0 (Wet)
  float norm = (float)(DRY_ADC - raw) / (float)(DRY_ADC - WET_ADC);
  norm = constrain(norm, 0.0f, 1.0f);

  // Square curve stretches lower spectrum (stops 80% sudden jump in dry pots)
  int pct = (int)(norm * norm * 100.0f);
  return constrain(pct, 0, 100);
}

void handlePacket(const uint8_t *data, int len) {
  if (len != (int)sizeof(struct_message)) return;
  if (!isArmed) return; // Ignore if out of scheduled surveillance slots

  struct_message m;
  memcpy(&m, data, sizeof(m));
  m.label[sizeof(m.label) - 1] = 0;

  unsigned long now = millis();
  portENTER_CRITICAL(&mux);
  bool isNew = !everDetected || strcmp(lastLabel, m.label) != 0 || (now - lastEventMs > EVENT_GAP_MS);
  strcpy(lastLabel, m.label);
  lastConf = m.confidence;
  lastDetectMs = now;
  everDetected = true;

  if (isNew) {
    for (int i = MAX_EVENTS - 1; i > 0; i--) events[i] = events[i - 1];
    strcpy(events[0].label, m.label);
    events[0].conf = m.confidence;
    events[0].ms = now;
    if (eventCount < MAX_EVENTS) eventCount++;
    lastEventMs = now;
    totalAlerts++;
    newAlertFlag = true;
  }
  portEXIT_CRITICAL(&mux);
}

#if ESP_ARDUINO_VERSION >= ESP_ARDUINO_VERSION_VAL(3, 0, 0)
void OnDataRecv(const esp_now_recv_info_t *info, const uint8_t *data, int len) { handlePacket(data, len); }
#else
void OnDataRecv(const uint8_t *mac, const uint8_t *data, int len) { handlePacket(data, len); }
#endif

// HTML Dashboard (Dark Glassmorphic UI with 12-Hour AM/PM 3-Slot Controls)
const char INDEX_HTML[] PROGMEM = R"KRUSHI(<!DOCTYPE html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>KrushiSense Gateway</title>
<style>
:root{--bg1:#07130d;--bg2:#0d2a1b;--card:rgba(255,255,255,.07);--line:rgba(255,255,255,.14);
--txt:#e9f5ee;--mut:#8fb09c;--ok:#3ddc84;--warn:#ffb74d;--bad:#ff5c5c;--acc:#4dd0e1}
*{box-sizing:border-box}
body{margin:0;min-height:100vh;color:var(--txt);font-family:system-ui,sans-serif;
background:linear-gradient(160deg,var(--bg1),var(--bg2));padding:14px}
.wrap{max-width:540px;margin:0 auto;display:grid;gap:14px}
header{display:flex;align-items:center;justify-content:space-between}
h1{font-size:20px;margin:0}h1 small{display:block;font-size:12px;color:var(--mut);font-weight:400}
.pill{display:flex;align-items:center;gap:6px;font-size:12px;color:var(--mut)}
.dot{width:9px;height:9px;border-radius:50%;background:var(--bad)}
.dot.on{background:var(--ok);box-shadow:0 0 8px var(--ok)}
.card{background:var(--card);border:1px solid var(--line);border-radius:18px;padding:16px;backdrop-filter:blur(10px)}
.card h2{margin:0 0 12px;font-size:12px;color:var(--mut);text-transform:uppercase;letter-spacing:1px}
.gauge{display:flex;align-items:center;gap:18px}
.g{position:relative;width:130px;height:130px;flex:none}
.g svg{transform:rotate(-90deg)}
.g .num{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
.g .num b{font-size:32px}.g .num i{font-style:normal;font-size:11px;color:var(--mut)}
#arc{transition:stroke-dashoffset .5s ease,stroke .4s}
.badge{padding:4px 10px;border-radius:99px;font-size:12px;font-weight:700;border:1px solid}
.badge.ok{color:var(--ok);border-color:var(--ok);background:rgba(61,220,132,.12)}
.badge.dry{color:var(--warn);border-color:var(--warn);background:rgba(255,183,77,.12)}
.alert{display:flex;align-items:center;gap:12px}
.ico{width:50px;height:50px;border-radius:14px;display:flex;align-items:center;justify-content:center;font-size:24px;background:rgba(255,255,255,.08)}
.alert.human .ico{background:rgba(255,92,92,.2)}.alert.animal .ico{background:rgba(255,183,77,.2)}
.bar{height:6px;border-radius:6px;background:rgba(255,255,255,.1);margin-top:8px;overflow:hidden}
.bar>div{height:100%;width:0;background:linear-gradient(90deg,var(--acc),var(--ok));transition:width .4s}
ul{list-style:none;margin:10px 0 0;padding:0;display:grid;gap:6px}
li{display:flex;justify-content:space-between;font-size:12px;padding:6px 10px;border-radius:10px;background:rgba(255,255,255,.04)}
.slot{display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:center;margin-bottom:8px}
select,input[type=time]{background:rgba(0,0,0,.3);border:1px solid var(--line);color:#fff;padding:6px;border-radius:8px;font-size:12px;color-scheme:dark}
.btn{background:var(--acc);color:#05130b;border:none;padding:8px 14px;border-radius:8px;font-weight:700;cursor:pointer;width:100%;margin-top:8px}
.row{margin:10px 0 4px;display:flex;justify-content:space-between;font-size:13px}
.sw{display:flex;justify-content:space-between;align-items:center;margin-top:12px}
.tg{position:relative;width:46px;height:26px}
.tg input{opacity:0;width:0;height:0}
.tg span{position:absolute;inset:0;border-radius:26px;background:rgba(255,255,255,.2);transition:.2s}
.tg span:before{content:"";position:absolute;width:20px;height:20px;left:3px;top:3px;border-radius:50%;background:#fff;transition:.2s}
.tg input:checked+span{background:var(--ok)}.tg input:checked+span:before{transform:translateX(20px)}
</style></head><body><div class="wrap">
<header>
  <h1>KrushiSense Hub<small>Field Gateway & Surveillance Control</small></h1>
  <div class="pill"><div class="dot" id="dot"></div><span id="conn">Syncing...</span></div>
</header>

<section class="card">
  <h2>Soil Moisture (मातीतील ओलावा)</h2>
  <div class="gauge">
    <div class="g">
      <svg width="130" height="130" viewBox="0 0 130 130">
        <circle cx="65" cy="65" r="54" fill="none" stroke="rgba(255,255,255,.1)" stroke-width="12"/>
        <circle id="arc" cx="65" cy="65" r="54" fill="none" stroke="#3ddc84" stroke-width="12" stroke-linecap="round"
                stroke-dasharray="339.29" stroke-dashoffset="339.29"/>
      </svg>
      <div class="num"><b id="mval">--</b><i>% Moisture</i></div>
    </div>
    <div>
      <span class="badge ok" id="mstat">--</span>
      <div style="font-size:12px;color:var(--mut);margin-top:8px" id="mnote">Initializing sensor...</div>
    </div>
  </div>
</section>

<section class="card">
  <h2>Intrusion Status <span id="armbadge" style="float:right;color:var(--bad)">STANDBY</span></h2>
  <div class="alert" id="alert">
    <div class="ico" id="aico">🛡️</div>
    <div style="flex:1">
      <div style="font-weight:700" id="atitle">All Clear</div>
      <div style="font-size:12px;color:var(--mut)" id="asub">No detection</div>
      <div class="bar"><div id="abar"></div></div>
    </div>
  </div>
  <ul id="events"></ul>
</section>

<section class="card">
  <h2>Surveillance Shifts (12-Hour AM/PM Slots)</h2>
  <div style="font-size:12px;color:var(--mut);margin-bottom:8px">Camera operates exclusively inside configured slots.</div>
  
  <div class="slot">
    <input type="time" id="s1_s" value="06:00">
    <input type="time" id="s1_e" value="09:00">
    <label class="tg"><input type="checkbox" id="s1_en" checked><span></span></label>
  </div>
  <div class="slot">
    <input type="time" id="s2_s" value="13:00">
    <input type="time" id="s2_e" value="15:00">
    <label class="tg"><input type="checkbox" id="s2_en" checked><span></span></label>
  </div>
  <div class="slot">
    <input type="time" id="s3_s" value="20:00">
    <input type="time" id="s3_e" value="04:00">
    <label class="tg"><input type="checkbox" id="s3_en" checked><span></span></label>
  </div>
  <button class="btn" onclick="saveSlots()">Save Shift Schedules</button>
</section>

<section class="card">
  <h2>Hardware Overrides</h2>
  <div class="sw"><span>Manual Siren / Buzzer Test</span>
    <label class="tg"><input type="checkbox" id="buzzer" onchange="toggleBuzzer(this.checked)"><span></span></label></div>
</section>
</div>

<script>
const $=id=>document.getElementById(id);
const C=339.29;

function minToTimeStr(m){
  let hr=Math.floor(m/60), min=m%60;
  return (hr<10?'0':'')+hr+':'+(min<10?'0':'')+min;
}
function timeStrToMin(s){
  let p=s.split(':');
  return parseInt(p[0])*60 + parseInt(p[1]);
}

function render(d){
  $('mval').textContent=d.moisture;
  $('arc').style.strokeDashoffset=C*(1-d.moisture/100);
  const dry=d.status==='dry';
  $('arc').style.stroke=dry?'#ffb74d':'#3ddc84';
  $('mstat').textContent=dry?'DRY':'OPTIMAL';
  $('mstat').className='badge '+(dry?'dry':'ok');$('mnote').textContent=dry?'Soil moisture low - water needed':'Optimal root zone moisture';

  $('armbadge').textContent=d.armed?'ACTIVE (ARMED)':'STANDBY';
  $('armbadge').style.color=d.armed?'var(--ok)':'var(--warn)';

  if(d.intruder.seen){
    $('alert').className='alert '+d.intruder.label;
    $('aico').textContent=d.intruder.label==='human'?'🧍':'🐾';
    $('atitle').textContent=d.intruder.label.toUpperCase()+' DETECTED!';$('asub').textContent=Math.round(d.intruder.conf)+'% confidence • '+d.intruder.age+'s ago';
    $('abar').style.width=Math.min(100,d.intruder.conf)+'%';
  }

  const ul=$('events'); ul.innerHTML='';
  d.events.forEach(e=>{
    const li=document.createElement('li');
    li.innerHTML=`<span>${e.label==='human'?'🧍 Human':'🐾 Animal'} (${Math.round(e.conf)}%)</span><span>${e.age}s ago</span>`;
    ul.appendChild(li);
  });
}

async function syncPhoneTime(){
  const now = new Date();
  const mins = now.getHours() * 60 + now.getMinutes();
  await fetch('/api/time?min=' + mins);
}

async function poll(){
  try{
    const r=await fetch('/api/data');
    const d=await r.json();
    render(d);
    $('dot').className='dot on';$('conn').textContent='Live (Hub Linked)';
  }catch(e){
    $('dot').className='dot';$('conn').textContent='Disconnected';
  }
}

async function saveSlots(){
  const p = new URLSearchParams({
    s1_s: timeStrToMin($('s1_s').value), s1_e: timeStrToMin($('s1_e').value), s1_en:$('s1_en').checked?1:0,
    s2_s: timeStrToMin($('s2_s').value), s2_e: timeStrToMin($('s2_e').value), s2_en:$('s2_en').checked?1:0,
    s3_s: timeStrToMin($('s3_s').value), s3_e: timeStrToMin($('s3_e').value), s3_en:$('s3_en').checked?1:0
  });
  await fetch('/api/slots', {method:'POST', body:p});
  alert('Shift schedules saved to Flash!');
}

async function toggleBuzzer(state){
  await fetch('/api/settings', {method:'POST', body:new URLSearchParams({buzzer: state?1:0})});
}

// Initial Sync & Loops
syncPhoneTime();
setInterval(syncPhoneTime, 60000); // Resync time every 1 min
setInterval(poll, 1500);
poll();
</script></body></html>)KRUSHI";

size_t buildDataJson(char *buf, size_t cap) {
  unsigned long now = millis();
  char label[16];
  Event copy[MAX_EVENTS];
  int n; float conf; bool ever; long age; unsigned long total;

  portENTER_CRITICAL(&mux);
  strcpy(label, lastLabel);
  conf = lastConf;
  ever = everDetected;
  age = ever ? (long)((now - lastDetectMs) / 1000UL) : -1;
  n = eventCount;
  memcpy(copy, events, sizeof(copy));
  total = totalAlerts;
  portEXIT_CRITICAL(&mux);

  char evs[320];
  size_t pos = 0;
  evs[0] = 0;
  for (int i = 0; i < n; i++) {
    int w = snprintf(evs + pos, sizeof(evs) - pos, "%s{\"label\":\"%s\",\"conf\":%.1f,\"age\":%lu}",
                     i ? "," : "", copy[i].label, copy[i].conf, (now - copy[i].ms) / 1000UL);
    if (w < 0 || (size_t)w >= sizeof(evs) - pos) break;
    pos += w;
  }

  bool dry = moisturePct < moistureThreshold;
  return snprintf(buf, cap,
    "{\"moisture\":%d,\"status\":\"%s\",\"armed\":%s,\"intruder\":{\"seen\":%s,\"label\":\"%s\",\"conf\":%.1f,\"age\":%ld},"
    "\"events\":[%s],\"total\":%lu,\"buzzer\":%s}",
    moisturePct, dry ? "dry" : "ok", isArmed ? "true" : "false", ever ? "true" : "false", label, conf, age,
    evs, total, manualBuzzer ? "true" : "false");
}

void sendData() {
  char buf[768];
  buildDataJson(buf, sizeof(buf));
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json", buf);
}

void handleTimeSync() {
  if (server.hasArg("min")) {
    currentMinutes = server.arg("min").toInt();
    lastClockSyncMs = millis();
  }
  server.send(200, "text/plain", "OK");
}

void handleSlotSave() {
  for (int i = 0; i < 3; i++) {
    String p = "s" + String(i + 1);
    slots[i].startMin = server.arg(p + "_s").toInt();
    slots[i].endMin   = server.arg(p + "_e").toInt();
    slots[i].enabled  = server.arg(p + "_en") == "1";
    
    prefs.putInt((p + "_s").c_str(), slots[i].startMin);
    prefs.putInt((p + "_e").c_str(), slots[i].endMin);
    prefs.putBool((p + "_en").c_str(), slots[i].enabled);
  }
  server.send(200, "text/plain", "OK");
}

void handleSettings() {
  if (server.hasArg("buzzer")) {
    manualBuzzer = server.arg("buzzer") == "1";
  }
  sendData();
}

void updateOutputs() {
  bool beeping = millis() < beepUntil;
  digitalWrite(BUZZER_PIN, (manualBuzzer || beeping) ? HIGH : LOW);
  digitalWrite(ALERT_LED_PIN, (manualBuzzer || beeping) ? HIGH : LOW);
}

// Broadcast Armed/Standby command to Camera Node
void broadcastSurveillanceMode(bool arm) {
  struct_command cmd;
  cmd.surveillance_active = arm;
  esp_now_send(camAddress, (uint8_t *)&cmd, sizeof(cmd));
}

void setup() {
  Serial.begin(115200);
  pinMode(ALERT_LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(MOISTURE_PIN, INPUT);
  analogReadResolution(12);

  prefs.begin("krushi", false);
  // Default shifts: Morning (06:00-09:00), Lunch (13:00-15:00), Night (20:00-04:00)
  slots[0].startMin = prefs.getInt("s1_s", 360);
  slots[0].endMin   = prefs.getInt("s1_e", 540);
  slots[0].enabled  = prefs.getBool("s1_en", true);

  slots[1].startMin = prefs.getInt("s2_s", 780);
  slots[1].endMin   = prefs.getInt("s2_e", 900);
  slots[1].enabled  = prefs.getBool("s2_en", true);

  slots[2].startMin = prefs.getInt("s3_s", 1200);
  slots[2].endMin   = prefs.getInt("s3_e", 240);
  slots[2].enabled  = prefs.getBool("s3_en", true);

  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(AP_SSID, AP_PASS, AP_CHANNEL);

  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, camAddress, 6);
    peerInfo.channel = AP_CHANNEL;
    peerInfo.encrypt = false;
    esp_now_add_peer(&peerInfo);
  }

  server.on("/", HTTP_GET, []() { server.send(200, "text/html", INDEX_HTML); });
  server.on("/api/data", HTTP_GET, sendData);
  server.on("/api/time", HTTP_GET, handleTimeSync);
  server.on("/api/slots", HTTP_POST, handleSlotSave);
  server.on("/api/settings", HTTP_POST, handleSettings);
  server.begin();

  moisturePct = readMoisturePercentage();
  lastMoistureCheck = millis();
}

void loop() {
  server.handleClient();
  unsigned long now = millis();

  // Internal Clock Calculation
  int activeMin = currentMinutes;
  if (currentMinutes >= 0) {
    unsigned long elapsed = (now - lastClockSyncMs) / 60000UL;
    activeMin = (currentMinutes + elapsed) % 1440;
  }

  // Determine Surveillance State
  bool shouldArm = false;
  if (activeMin >= 0) {
    for (int i = 0; i < 3; i++) {
      if (slots[i].enabled && isTimeInSlot(activeMin, slots[i].startMin, slots[i].endMin)) {
        shouldArm = true;
        break;
      }
    }
  } else {
    shouldArm = true; // Default to armed if clock not yet synced
  }

  static unsigned long lastBroadcast = 0;
  if (shouldArm != isArmed || (now - lastBroadcast > 3000)) {
    isArmed = shouldArm;
    broadcastSurveillanceMode(isArmed);
    lastBroadcast = now;
  }

  // Read Moisture Non-blocking
  if (now - lastMoistureCheck >= moistureIntervalMs) {
    lastMoistureCheck = now;
    moisturePct = readMoisturePercentage();
  }

  if (newAlertFlag) {
    newAlertFlag = false;
    beepUntil = millis() + BEEP_MS;
  }

  updateOutputs();
  delay(2);
}