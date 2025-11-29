/*
 * pc_power_controller.ino
 * ESP32 - PC Power Controller (embedded web UI)
 *
 * - Embedded index.html (user-provided) served at "/"
 * - GET  /state       -> JSON { power:true/false, wifi:"ok"/"nok", autoOn:"HH:MM", autoOff:"HH:MM", time: "HH:MM:SS" }
 * - POST /press       -> triggers relay pulse (power button)
 * - POST /setAutoOn   -> body contains JSON with "time":"HH:MM"
 * - POST /setAutoOff  -> same as above
 * - WebSocket server on port 81, path "/ws" (ws://<IP>:81/ws)
 * - NTP time sync, Preferences persistence for auto times
 * - Relay pin and ADC pin configurable below
 *
 * FELHASZNÁLÓI ADATOK KÜLÖN secrets.h FÁJLBÓL TÖLTVE.
 */

#include <WiFi.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <Preferences.h>
#include <time.h>
#include "secrets.h" //  Az érzékeny adatok (SSID, Jelszó) innen töltődnek be

//
// ========== USER CONFIG ==========
#define RELAY_PIN 25   //  GPIO controlling relay IN (active HIGH)
#define LED_INPUT 34   // [cite: 4] ADC pin reading power LED voltage (via divider)

// Magyarországi időzóna (CET/CEST), automatikus DST váltás
// [cite: 4, 88] CET-1CEST,M3.4.0/2,M10.4.0/3 => Közép-Európai Idő, DST kezdete Március 4. vasárnap 2:00, DST vége Október 4. vasárnap 3:00
const char* TIMEZONE = "CET-1CEST,M3.4.0/2,M10.4.0/3";
const char* NTP_SERVER = "pool.ntp.org"; //

const int ADC_THRESHOLD = 800;   // [cite: 4] calibrate (0-4095)
const unsigned long PULSE_MS = 1000; // [cite: 5] relay pulse duration (ms)

//
// ========== GLOBALS ==========
WebServer server(80); // [cite: 5]
WebSocketsServer webSocket = WebSocketsServer(81, "/ws", 4); // [cite: 6]
Preferences prefs; // [cite: 6]

bool lastPowerState = false; // [cite: 7]
bool wifiOk = false; // [cite: 7]

String autoOnTime = "";   // "HH:MM" [cite: 7]
String autoOffTime = ""; // [cite: 8]
unsigned long lastBroadcast = 0; // [cite: 8]
unsigned long lastAutoCheck = 0; // [cite: 8]
unsigned long pulseStartTime = 0; // Relé pulzus időzítéséhez
bool isPulsing = false; // Jelzi, ha a relé épp pulzál
const unsigned long BROADCAST_INTERVAL = 2000; // 2 másodperc
const unsigned long AUTO_CHECK_INTERVAL = 1000; // 1 másodperc

int lastOnTriggeredMinute  = -1; // [cite: 8]
int lastOffTriggeredMinute = -1; // [cite: 8]

//
// ========== EMBEDDED WEB UI (minimal changes for time display) ==========
// A JS részt úgy módosítottam, hogy a szerver által küldött "time" mezőt is megjelenítse.
const char INDEX_HTML[] PROGMEM = R"HTML(
<!doctype html>
<html lang="hu">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>PC Power Controller</title>
<style>
  :root{
    --bg:#0f1724;
    --card:#0b1220;
    --accent:#1976d2; /* kék */
    --accent-glow: rgba(25,118,210,0.22);
    --muted:#9aa6b2;
    --ok:#2ee6a3;
    --danger:#ff6b6b;
    --glass: rgba(255,255,255,0.03);
    font-family: Inter, ui-sans-serif, system-ui, -apple-system, "Segoe UI", Roboto, "Helvetica Neue", Arial;
  }
  html,body{height:100%;margin:0;background:linear-gradient(180deg,#08111a 0%, #07101a 100%);color:#e6eef6}
  .wrap{max-width:920px;margin:28px auto;padding:18px;display:grid;grid-template-columns: 1fr 360px;gap:18px}
  .card{background:linear-gradient(180deg, rgba(255,255,255,0.02), rgba(255,255,255,0.01));border-radius:14px;padding:18px;box-shadow:0 6px 24px rgba(2,6,23,0.6);border:1px solid rgba(255,255,255,0.03)}

header{grid-column:1/-1;display:flex;align-items:center;justify-content:space-between;gap:12px;margin-bottom:4px}
  h1{font-size:20px;margin:0}
  .statusbar{display:flex;gap:12px;align-items:center}
  .pill{padding:6px 10px;border-radius:999px;font-size:13px;background:var(--glass);color:var(--muted);display:inline-flex;align-items:center;gap:8px;border:1px solid rgba(255,255,255,0.02)}
  .pill .dot{width:10px;height:10px;border-radius:50%}
  .dot.on{background:var(--accent);box-shadow:0 0 14px var(--accent-glow)}
  .dot.off{background:#21313c}
  /* POWER BUTTON */
  .power-area{display:flex;flex-direction:column;align-items:center;justify-content:center;height:420px}
  .power-btn{width:250px;height:250px;border-radius:999px;display:flex;align-items:center;justify-content:center;cursor:pointer;position:relative;
      background: radial-gradient(circle at 30% 20%, rgba(255,255,255,0.02), transparent 30%), linear-gradient(180deg, rgba(255,255,255,0.01), transparent);
      border:1px solid rgba(255,255,255,0.04);box-shadow: inset 0 6px 20px rgba(0,0,0,0.6);}
      .power-ring{position:absolute;inset: -12px;border-radius:999px;display:block;filter:blur(8px);opacity:0;transition:0.35s ease}
      .power-ring.on{background: radial-gradient(circle at center, rgba(25,118,210,0.28), rgba(25,118,210,0.12) 30%, transparent 40%);opacity:1}
      .power-icon{width:110px;height:110px;display:flex;align-items:center;justify-content:center;transition:transform .14s ease}
      .power-btn:active .power-icon{transform:scale(0.96)}
      .power-label{margin-top:14px;color:var(--muted);font-size:14px}
      .small{font-size:13px;color:var(--muted)}
      /* Controls */
      .controls{display:flex;flex-direction:column;gap:12px}
      .card .row{display:flex;align-items:center;justify-content:space-between;gap:12px}
      .time-input{display:flex;flex-direction:column;gap:6px}
      input[type="time"]{background:transparent;border:1px solid rgba(255,255,255,0.04);padding:8px 10px;border-radius:8px;color:inherit;outline:none}
      button.btn{padding:10px 12px;border-radius:10px;border:0;cursor:pointer;background:linear-gradient(180deg,var(--accent),#115fa8);color:white;font-weight:600}

      button.ghost{background:transparent;border:1px solid rgba(255,255,255,0.04);color:var(--muted)}
      .meta{font-size:13px;color:var(--muted);display:flex;flex-direction:column;gap:6px}
      .log{max-height:150px;overflow:auto;padding:8px;background:rgba(255,255,255,0.015);border-radius:8px;font-family:monospace;font-size:13px;color:var(--muted)}
      footer{grid-column:1/-1;text-align:center;color:var(--muted);font-size:13px;margin-top:8px}
      @media (max-width:880px){.wrap{grid-template-columns:1fr;padding:12px}.power-area{height:360px}}
      </style>
      </head>
      <body>
      <div class="wrap">
      <header>
      <h1>PC Power Controller</h1>
      <div class="statusbar">
      <div class="pill" id="wifiPill">WiFi: <strong id="wifiState" style="margin-left:8px">--</strong></div>
      <div class="pill">Állapot: <span style="margin-left:8px" id="powerPill"><span
      class="dot off"></span> <span id="powerText">--</span></span></div>
      <div class="pill">Idő: <strong id="timeNow" style="margin-left:8px">--:--:--</strong></div>
      </div>
      </header>

      <section class="card power-area">
      <div class="power-btn" id="powerBtn" title="Kattints bekapcsoláshoz / kikapcsoláshoz">
      <span class="power-ring" id="powerRing"></span>
      <svg class="power-icon" viewBox="0 0 100 100" xmlns="http://www.w3.org/2000/svg" aria-hidden="true">
      <g fill="none" stroke="#cfe9ff" stroke-width="6" stroke-linecap="round" stroke-linejoin="round">
      <path d="M50 14v26" id="pwr-line" stroke-linecap="round"/>
      <path d="M73 26a28 28 0 1 1-46
      0" id="pwr-ring"/>
      </g>
      </svg>
      </div>
      <div class="power-label" id="powerLabel">Gép állapota: <strong id="powerLabelState">--</strong></div>
      <div class="small">Kattints a gombra a távoli "gombnyomás" küldéséhez.</div>
      </section>

      <aside class="card controls">
      <div class="meta">
      <div>Automata időzítés beállítása</div>
      <div class="small">Az ESP-re elküldve aktiválódik a beállítás (szerver oldalon kell kezelni).</div>
      </div>


      <div class="row">
      <div style="flex:1">
      <label class="small">Auto ON</label>
      <div class="time-input">
      <input type="time" id="timeOn" />
      </div>
      </div>
      <div style="display:flex;flex-direction:column;gap:8px;align-items:flex-end">
      <button class="btn" id="saveOn">Mentés</button>
      </div>
      </div>

      <div class="row">
      <div style="flex:1">
      <label class="small">Auto OFF</label>
      <div
      class="time-input">
      <input type="time" id="timeOff" />
      </div>
      </div>
      <div style="display:flex;flex-direction:column;gap:8px;align-items:flex-end">
      <button class="btn" id="saveOff">Mentés</button>
      </div>
      </div>

      <div>
      <label class="small">Kézi vezérlés</label>
      <div style="display:flex;gap:8px;margin-top:8px">
      <button class="ghost" id="refresh">Frissítés</button>
      <button class="btn" id="pressBtn">Gombnyomás küldése</button>
      </div>
      </div>


      <div style="margin-top:10px">
      <label class="small">Napló</label>
      <div class="log" id="logArea">-- napló --</div>
      </div>
      </aside>

      <footer>Csatlakozás az eszközhöz: a web UI megpróbálja a <code>ws://[host]/ws</code> WebSocket-et, ellenkező esetben HTTP végpontokat használ.
      (GET /state, POST /press, POST /setAutoOn, POST /setAutoOff)</footer>
      </div>

      <script>
      /*
       * Feltételezett API:
       * - GET  /state       -> { "power": true/false, "wifi": "ok"/"nok", "autoOn":"08:00", "autoOff":"23:30", "time":"11:20:37" }
       * - POST /press       -> {}  (visszatérési state)
       * - POST /setAutoOn
       * -> { "time": "HH:MM" }
       * - POST /setAutoOff  -> { "time": "HH:MM" }
       *
       * Ha a szerver WebSocketet támogat: ws://HOST/ws küld: { "type":"state","power":true, ... }
       */

      const hostBase = (() => {
          // alap: ugyanaz a host ahonnan a UI jött
          return `${location.protocol}//${location.host}`;

      })();
      const api = {
          state: '/state',
          press: '/press',
          setOn: '/setAutoOn',
          setOff: '/setAutoOff',
          wsPath: (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws'
      };
      const el = {
          powerBtn: document.getElementById('powerBtn'),
          powerRing: document.getElementById('powerRing'),
          powerText: document.getElementById('powerText'),
          powerLabelState: document.getElementById('powerLabelState'),
          powerPillDot: document.querySelector('#powerPill .dot'),
          powerLabel: document.getElementById('powerLabel'),
          wifiState: document.getElementById('wifiState'),
          timeNow: document.getElementById('timeNow'),

          timeOn: document.getElementById('timeOn'),
          timeOff: document.getElementById('timeOff'),
          saveOn: document.getElementById('saveOn'),
          saveOff: document.getElementById('saveOff'),
          pressBtn: document.getElementById('pressBtn'),
          refresh: document.getElementById('refresh'),
          logArea: document.getElementById('logArea'),
      };
      let lastState = { power: false, wifi: 'nok' };
      let ws = null;
      let pollTimer = null;
      function log(msg){
          const t = new Date().toLocaleTimeString();
          el.logArea.textContent = `[${t}] ${msg}\n` + el.logArea.textContent;
      }

      // UI update
      function updateUI(state){
          lastState = state;
          // power ring + text
          if(state.power){
              el.powerRing.classList.add('on');
              el.powerPillDot.classList.remove('off'); el.powerPillDot.classList.add('on');
              el.powerText.textContent = 'BE';
              el.powerLabelState.textContent = 'BE';
          } else {
              el.powerRing.classList.remove('on');
              el.powerPillDot.classList.remove('on'); el.powerPillDot.classList.add('off');
              el.powerText.textContent = 'KI';
              el.powerLabelState.textContent = 'KI';
          }
          // wifi
          el.wifiState.textContent = state.wifi ||
          'ismeretlen';
          // times
          if(state.autoOn) el.timeOn.value = state.autoOn;
          if(state.autoOff) el.timeOff.value = state.autoOff;
          // current time from server
          if(state.time) el.timeNow.textContent = state.time;
      }

      // HTTP helpers
      async function httpGet(path){
          try {
              const res = await fetch(path, { cache:'no-store' });
              if(!res.ok) throw new Error('HTTP ' + res.status);
              return await res.json();
          } catch(e) {
              log('GET hiba: ' + e.message);
              throw e;
          }
      }
      async function httpPost(path, body){
          try {
              const res = await fetch(path, {
                  method:'POST',
                  headers: {'Content-Type':'application/json'},

                  body: JSON.stringify(body || {})
              });
              if(!res.ok) throw new Error('HTTP ' + res.status);
              return await res.json();
          } catch(e) {
              log('POST hiba: ' + e.message);
              throw e;
          }
      }

      // actions
      async function refreshState(){
          try {
              const s = await httpGet(api.state);
              updateUI(s);
              log('Állapot frissítve');
          } catch(e){
              log('Állapot lekérdezés sikertelen');
          }
      }

      async function sendPress(){
          try {
              el.pressBtn.disabled = true;
              log('Gombnyomás küldése...');
              const r = await httpPost(api.press, {});
              if(r && typeof r.power !== 'undefined') updateUI(r);
              log('Gombnyomás elküldve');
          } catch(e){
              log('Gombnyomás sikertelen');
          } finally {
              el.pressBtn.disabled = false;
          }
      }

      async function saveAutoOn(){
          const t = el.timeOn.value;
          if(!t){ alert('Válassz egy időt!'); return; }
          try {
              el.saveOn.disabled = true;
              await httpPost(api.setOn, { time: t });
              log('Auto ON mentve: ' + t);
          } catch(e){
              log('Auto ON mentése sikertelen');
          } finally {
              el.saveOn.disabled = false;
          }
      }

      async function saveAutoOff(){
          const t = el.timeOff.value;
          if(!t){ alert('Válassz egy időt!'); return; }
          try {
              el.saveOff.disabled = true;
              await httpPost(api.setOff, { time: t });
              log('Auto OFF mentve: ' + t);
          } catch(e){
              log('Auto OFF mentése sikertelen');
          } finally {
              el.saveOff.disabled = false;
          }
      }

      // WebSocket (ha van)
      function tryWebSocket(){
          try {
              ws = new WebSocket(api.wsPath);
          } catch(e){
              log('WebSocket nem indult: ' + e.message);
              ws = null;
              return;
          }
          ws.onopen = () => {
              log('WebSocket csatlakoztatva');
          };
          ws.onmessage = (evt) => {
              try {
                  const data = JSON.parse(evt.data);
                  if(data.type === 'state'){ //  A szerver küldi már a "type":"state" mezőt
                      updateUI(data);
                  } else if(data.type === 'log'){
                      log(data.msg || JSON.stringify(data));
                  } else {
                      // fallback for older/simpler state objects
                      if(typeof data.power !== 'undefined') updateUI(data);
                  }
              } catch(e){
                  console.log('ws parse err', e);
              }
          };
          ws.onclose = () => {
              log('WebSocket bontva, ellenőrzés pollingra');
              ws = null;
              startPolling();
              setTimeout(tryWebSocket, 7000);
          };
          ws.onerror = (e)=>{
              console.warn('ws err', e);
          };
      }

      // Polling
      function startPolling(){
          if(pollTimer) return;
          pollTimer = setInterval(()=> {
              refreshState();
          }, 5000);
          refreshState();
      }
      function stopPolling(){
          if(pollTimer){ clearInterval(pollTimer);
              pollTimer = null; }
      }

      // UX events
      el.powerBtn.addEventListener('click', async ()=>{
          el.powerBtn.classList.add('active');
          await sendPress();
          setTimeout(()=> el.powerBtn.classList.remove('active'), 120);
      });
      el.pressBtn.addEventListener('click', sendPress);
      el.refresh.addEventListener('click', refreshState);
      el.saveOn.addEventListener('click', saveAutoOn);
      el.saveOff.addEventListener('click', saveAutoOff);

      // A kliens oldali óra most már nem szükséges, a szerver küldi az időt.
      el.timeNow.textContent = '--:--:--';

      // init
      (function init(){
          log('UI betöltve, inicializálás...');
          tryWebSocket();
          setTimeout(()=> {
              if(!ws) startPolling();
          }, 2000);
      })();
      </script>
      </body>
      </html>
      )HTML";

      //
      // ========== HELPERS ==========
      String makeStateJson() {
          struct tm timeinfo;
          String curTime = "--:--:--";
          if (getLocalTime(&timeinfo)) {
              char timeStr[9];
              strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
              curTime = timeStr;
          }

          String js = "{";
          js += "\"type\":\"state\","; //  Hozzáadva a kliens oldali WS kezeléshez
          js += "\"power\":" + String(lastPowerState ? "true" : "false") + ",";
          js += "\"wifi\":\"" + String(wifiOk ? "ok" : "nok") + "\",";
          js += "\"autoOn\":\"" + (autoOnTime.length() ? autoOnTime : "") + "\","; // [cite: 73]
          js += "\"autoOff\":\"" + (autoOffTime.length() ? autoOffTime : "") + "\","; // [cite: 73]
          js += "\"time\":\"" + curTime + "\""; // Szerver aktuális ideje
          js += "}";
          return js;
      }

      // JSON parsing segítő függvény
      String extractHHMMFromJson(const String &body) {
          // Egyszerűsített JSON keresés a "time":"HH:MM" mintára
          int start = body.indexOf("\"time\"");
          if (start == -1) return "";
          start = body.indexOf(':', start);
          if (start == -1) return "";
          start = body.indexOf('"', start) + 1;
          if (start == 0) return "";

          if (start + 5 <= body.length() && body[start+2] == ':') {
              return body.substring(start, start + 5);
          }
          return "";
      }

      bool parseHHMM(const String &s, int &h, int &m) {
          if (s.length() != 5) return false; // [cite: 75]
          if (!isDigit(s[0]) || !isDigit(s[1]) || s[2] != ':' || !isDigit(s[3]) || !isDigit(s[4])) return false; // [cite: 76]
          h = (s[0]-'0')*10 + (s[1]-'0'); // [cite: 76]
          m = (s[3]-'0')*10 + (s[4]-'0'); // [cite: 77]
          return (h >= 0 && h < 24 && m >= 0 && m < 60); // [cite: 77]
      }

      void broadcastState() {
          String s = makeStateJson();
          webSocket.broadcastTXT(s); // [cite: 79]
      }

      //
      // ========== HW actions ==========
      bool readPcPowerState() {
          int v = analogRead(LED_INPUT); // [cite: 80]
          return v > ADC_THRESHOLD; // [cite: 80]
      }

      // A relé pulzust most már a loop() kezeli. Nem blokkoló.
      void pressPowerButton() {
          if (!isPulsing) {
              digitalWrite(RELAY_PIN, HIGH); // [cite: 81]
              pulseStartTime = millis();
              isPulsing = true;
          }
      }

      // Kezeli a relé kikapcsolását a pulzus letelte után
      void checkPulseEnd() {
          if (isPulsing && millis() - pulseStartTime >= PULSE_MS) {
              digitalWrite(RELAY_PIN, LOW); // [cite: 82]
              isPulsing = false;
              // A pulzus vége után olvassuk be az új állapotot és broadcastoljuk
              lastPowerState = readPcPowerState();
              broadcastState();
          }
      }

      //
      // ========== NETWORK / NTP ==========
      void startWiFi() {
          WiFi.mode(WIFI_STA); // [cite: 83]
          WiFi.begin(WIFI_SSID, WIFI_PASS); // [cite: 83]
          unsigned long start = millis();
          Serial.printf("WiFi connecting to '%s' ...\n", WIFI_SSID); // [cite: 83]
          while (WiFi.status() != WL_CONNECTED && millis() - start < 25000) { // [cite: 84]
              delay(250); // [cite: 84]
              Serial.print("."); // [cite: 85]
          }
          Serial.println(); // [cite: 85]
          if (WiFi.status() == WL_CONNECTED) { // [cite: 86]
              Serial.print("WiFi connected, IP: "); // [cite: 86]
              Serial.println(WiFi.localIP()); // [cite: 87]
              wifiOk = true; // [cite: 87]
          } else {
              Serial.println("WiFi failed"); //
              wifiOk = false; //
          }
      }

      void startNTP() {
          setenv("TZ", TIMEZONE, 1); // Posix time zone string beállítása
          tzset();
          configTime(0, 0, NTP_SERVER); //
          Serial.printf("NTP sync requested for TZ: %s...\n", TIMEZONE); // [cite: 89]

          struct tm ti;
          for (int i = 0; i < 8; ++i) { // [cite: 89]
              if (getLocalTime(&ti)) { // [cite: 89]
                  Serial.println("Got time from NTP"); // [cite: 90]
                  break; // [cite: 90]
              }
              delay(400); // [cite: 90]
          }
      }

      //
      // ========== HTTP handlers ==========
      void handleRoot() {
          server.sendHeader("Content-Encoding", ""); // [cite: 92]
          server.send_P(200, "text/html", INDEX_HTML); // [cite: 92]
      }

      void handleGetState() {
          lastPowerState = readPcPowerState(); // [cite: 93]
          String js = makeStateJson(); // [cite: 94]
          server.send(200, "application/json", js); // [cite: 94]
      }

      void handlePostPress() {
          pressPowerButton(); // [cite: 94] Az állapotfrissítés és broadcast a checkPulseEnd()-be került át
          String js = makeStateJson(); // [cite: 95] Azonnali válasz az utolsó állapottal
          server.send(200, "application/json", js);
      }

      void handleSetAutoOn() {
          String body = server.arg("plain"); // [cite: 96]
          String t = extractHHMMFromJson(body); // JSON alapú HH:MM keresés

          if (t.length() == 0) {
              server.send(400, "text/plain", "Time not found in JSON body or invalid format."); // [cite: 97]
              return; // [cite: 98]
          }
          int hh, mm; // [cite: 98]
          if (!parseHHMM(t, hh, mm)) { // [cite: 99]
              server.send(400, "text/plain", "Invalid time format (HH:MM)."); // [cite: 99]
              return; // [cite: 100]
          }
          autoOnTime = t; // [cite: 100]
          prefs.putString("auto_on", autoOnTime); // [cite: 100]

          String js = makeStateJson(); // [cite: 101]
          server.send(200, "application/json", js); // [cite: 101]
          broadcastState(); // [cite: 101]
          Serial.printf("Auto ON set to %s\n", t.c_str()); // [cite: 102]
      }

      void handleSetAutoOff() {
          String body = server.arg("plain"); // [cite: 102]
          String t = extractHHMMFromJson(body); // JSON alapú HH:MM keresés

          if (t.length() == 0) {
              server.send(400, "text/plain", "Time not found in JSON body or invalid format."); // [cite: 103]
              return; // [cite: 104]
          }
          int hh, mm; // [cite: 104]
          if (!parseHHMM(t, hh, mm)) { // [cite: 105]
              server.send(400, "text/plain", "Invalid time format (HH:MM)."); // [cite: 106]
              return; // [cite: 106]
          }
          autoOffTime = t; // [cite: 106]
          prefs.putString("auto_off", autoOffTime); // [cite: 106]

          String js = makeStateJson(); // [cite: 107]
          server.send(200, "application/json", js); // [cite: 107]
          broadcastState(); // [cite: 107]
          Serial.printf("Auto OFF set to %s\n", t.c_str()); // [cite: 108]
      }

      //
      // ========== WebSocket event ==========
      void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
          if (type == WStype_CONNECTED) {
              IPAddress ip = webSocket.remoteIP(num); // [cite: 108]
              Serial.printf("WS client %u connected from %s\n", num, ip.toString().c_str()); // [cite: 109]
              webSocket.sendTXT(num, makeStateJson()); // [cite: 109]
          } else if (type == WStype_DISCONNECTED) {
              Serial.printf("WS client %u disconnected\n", num); // [cite: 110]
          } else if (type == WStype_TEXT) {
              String msg = String((char*)payload); // [cite: 111]
              Serial.printf("WS msg from %u: %s\n", num, msg.c_str()); // [cite: 112]
              // If client sends "press"
              if (msg.equalsIgnoreCase("press")) {
                  pressPowerButton(); // [cite: 112]
                  // Az állapotfrissítés és broadcast a checkPulseEnd()-be került át
              }
          }
      }

      //
      // ========== Setup & Loop ==========
      void setup() {
          Serial.begin(115200); // [cite: 113]
          delay(50); // [cite: 114]

          pinMode(RELAY_PIN, OUTPUT); // [cite: 114]
          digitalWrite(RELAY_PIN, LOW); // [cite: 114]

          pinMode(LED_INPUT, INPUT); // [cite: 114]

          prefs.begin("pcctrl", false); // [cite: 114]
          autoOnTime  = prefs.getString("auto_on", ""); // [cite: 114]
          autoOffTime = prefs.getString("auto_off", ""); // [cite: 114]
          Serial.printf("Loaded autoOn='%s' autoOff='%s'\n", autoOnTime.c_str(), autoOffTime.c_str()); // [cite: 115]

          startWiFi(); // [cite: 115]
          if (wifiOk) startNTP(); // [cite: 115]

          // HTTP routes
          server.on("/", HTTP_GET, handleRoot); // [cite: 115]
          server.on("/state", HTTP_GET, handleGetState); // [cite: 116]
          server.on("/press", HTTP_POST, handlePostPress); // [cite: 116]
          server.on("/setAutoOn", HTTP_POST, handleSetAutoOn); // [cite: 116]
          server.on("/setAutoOff", HTTP_POST, handleSetAutoOff); // [cite: 116]

          server.begin(); // [cite: 117]
          Serial.println("HTTP server started on port 80"); // [cite: 117]
          // WebSocket
          webSocket.begin(); // [cite: 117]
          webSocket.onEvent(webSocketEvent); // [cite: 118]
          Serial.println("WebSocket server started on port 81 (path /ws if supported)"); // [cite: 118]

          lastPowerState = readPcPowerState(); // [cite: 119]
          lastBroadcast = millis(); // [cite: 119]
          lastAutoCheck = millis(); // [cite: 119]
      }

      void checkAutoTimers() {
          struct tm timeinfo; // [cite: 119]
          if (!getLocalTime(&timeinfo)) return; // [cite: 120] no valid time yet

          int curMin = timeinfo.tm_hour * 60 + timeinfo.tm_min; // [cite: 120]

          // --- Auto ON ellenőrzés ---
          if (autoOnTime.length()) { // [cite: 121]
              int hh, mm; // [cite: 122]
              if (parseHHMM(autoOnTime, hh, mm)) { // [cite: 122]
                  int target = hh*60 + mm; // [cite: 123]
                  if (target == curMin && lastOnTriggeredMinute != curMin) { // [cite: 123]
                      if (!readPcPowerState()) { // [cite: 124]
                          Serial.printf("Auto ON triggered at %02d:%02d\n", hh, mm); // [cite: 124]
                          pressPowerButton(); // [cite: 124]
                          // Az állapotfrissítés a checkPulseEnd()-be került át
                      } else {
                          Serial.println("Auto ON: PC already ON, skipping"); // [cite: 125]
                      }
                      lastOnTriggeredMinute = curMin; // [cite: 126]
                  }
              }
          } else {
              // ha az időzítő ki van kapcsolva, reseteljük a triggert
              lastOnTriggeredMinute = -1;
          }

          // --- Auto OFF ellenőrzés ---
          if (autoOffTime.length()) { // [cite: 127]
              int hh, mm; // [cite: 127]
              if (parseHHMM(autoOffTime, hh, mm)) { // [cite: 128]
                  int target = hh*60 + mm; // [cite: 128]
                  if (target == curMin && lastOffTriggeredMinute != curMin) { // [cite: 128]
                      if (readPcPowerState()) { // [cite: 129]
                          Serial.printf("Auto OFF triggered at %02d:%02d\n", hh, mm); // [cite: 129]
                          pressPowerButton(); // [cite: 129]
                          // Az állapotfrissítés a checkPulseEnd()-be került át
                      } else {
                          Serial.println("Auto OFF: PC already OFF, skipping"); // [cite: 130]
                      }
                      lastOffTriggeredMinute = curMin; // [cite: 131]
                  }
              }
          } else {
              // ha az időzítő ki van kapcsolva, reseteljük a triggert
              lastOffTriggeredMinute = -1;
          }
      }

      void loop() {
          server.handleClient(); // [cite: 131]
          webSocket.loop(); // [cite: 132]
          checkPulseEnd(); // Relé pulzus befejezésének ellenőrzése

          unsigned long now = millis();

          // Állapot broadcast
          if (now - lastBroadcast >= BROADCAST_INTERVAL) {
              lastBroadcast = now; // [cite: 133]
              // Az állapotot csak a broadcast előtt olvassuk be, ha nincs pulzálás.
              // Pulzálás végén is történik beolvasás/broadcast.
              if(!isPulsing) {
                  lastPowerState = readPcPowerState(); // [cite: 133]
                  broadcastState();
              }
          }

          // Automata időzítők
          if (now - lastAutoCheck >= AUTO_CHECK_INTERVAL) {
              lastAutoCheck = now; // [cite: 134]
              checkAutoTimers(); // [cite: 134]
          }

          delay(10); // [cite: 134]
      }
