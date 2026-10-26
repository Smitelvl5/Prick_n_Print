#include "TestServer.h"
#include "Logger.h"
#include "secrets.h"
#include "WebTheme.h"
#include "version.h"
#include <WiFi.h>
#include <esp_system.h>

static const unsigned long TEST_PUMP_DURATION_MS = 800;
static const unsigned long TEST_LED_DURATION_MS = 1000;

// Defined in main.cpp: cancels the ESP-NOW-driven test-pulse timer so it
// doesn't also try to shut off the actuator this server just started testing.
void cancelEspNowTest();

TestServer::TestServer(HardwareAbstraction* hw, PrinterService* printer, bool* autoDispense, EspNowService* espNow)
    : server_(8081), hardware_(hw), printerService_(printer), autoDispenseEnabled_(autoDispense), espNow_(espNow) {
    // Reuses WEB_PASSWORD (shared Main+TZT web/API secret from secrets.h) as the bench-test key
    // so there's one credential to manage, not a new one per debug endpoint.
    security_.setApiKey(WEB_PASSWORD);
    security_.enableAuth(true);
    security_.setRateLimit(120);  // generous: this is LAN bench-test tooling, not the public API
}

bool TestServer::isAuthorized() {
    String key = server_.hasHeader("X-API-Key") ? server_.header("X-API-Key") : server_.arg("key");
    if (!security_.isAuthorized(server_.client().remoteIP().toString(), key)) {
        sendError(401, "unauthorized");
        return false;
    }
    return true;
}

void TestServer::sendOk(const String& extraJsonFields) {
    String body = "{\"ok\":true";
    if (extraJsonFields.length() > 0) body += "," + extraJsonFields;
    body += "}";
    server_.send(200, "application/json", body);
}

void TestServer::sendError(int httpStatus, const String& message) {
    String body = "{\"ok\":false,\"error\":\"" + message + "\"}";
    server_.send(httpStatus, "application/json", body);
}

// ============================================================================
// Printer
// ============================================================================

void TestServer::handlePrinterTest() {
    if (!isAuthorized()) return;
    if (!printerService_) { sendError(503, "printer service not initialized"); return; }
    bool ok = printerService_->printTest();
    if (!ok) { sendError(503, "printer not ready"); return; }
    sendOk();
}

void TestServer::handlePrinterRaw() {
    if (!isAuthorized()) return;
    if (!hardware_ || !hardware_->printerAvailable()) { sendError(503, "printer not available"); return; }
    hardware_->printerPrintln("RAW TEST 0123456789 ABCXYZ");
    sendOk();
}

void TestServer::handlePrinterReceipt() {
    if (!isAuthorized()) return;
    if (!printerService_) { sendError(503, "printer service not initialized"); return; }
    bool ok = printerService_->printReceipt("Hardware test receipt", true, 0);
    if (!ok) { sendError(503, "printer not ready"); return; }
    sendOk();
}

void TestServer::handlePrinterFeed() {
    if (!isAuthorized()) return;
    if (!hardware_ || !hardware_->printerAvailable()) { sendError(503, "printer not available"); return; }
    if (server_.hasArg("lines") && server_.arg("lines").toInt() <= 0) { sendError(400, "lines must be a positive integer"); return; }
    int lines = server_.hasArg("lines") ? server_.arg("lines").toInt() : 3;
    if (lines > 50) lines = 50;  // safety clamp
    hardware_->printerFeed((uint8_t)lines);
    sendOk("\"lines\":" + String(lines));
}

// ============================================================================
// Pump
// ============================================================================

void TestServer::handlePump() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (server_.hasArg("ms") && server_.arg("ms").toInt() <= 0) { sendError(400, "ms must be a positive integer"); return; }
    unsigned long ms = server_.hasArg("ms") ? server_.arg("ms").toInt() : TEST_PUMP_DURATION_MS;
    if (ms > 5000) { sendError(400, "ms must be <= 5000"); return; }
    if (!hardware_->checkCooldown()) { sendError(503, "pump on cooldown, try again shortly"); return; }
    cancelEspNowTest();
    if (!hardware_->startPump()) { sendError(503, "pump start failed"); return; }
    activeTest_ = 1;
    testStartMs_ = millis();
    testDurationMs_ = ms;
    sendOk("\"ms\":" + String(ms));
}

// ============================================================================
// LED
// ============================================================================

void TestServer::handleLed() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (server_.hasArg("level") && (server_.arg("level").toInt() < 0 || server_.arg("level").toInt() > 255)) {
        sendError(400, "level must be 0-255"); return;
    }
    if (server_.hasArg("ms") && server_.arg("ms").toInt() <= 0) { sendError(400, "ms must be a positive integer"); return; }
    int level = server_.hasArg("level") ? server_.arg("level").toInt() : 255;
    unsigned long ms = server_.hasArg("ms") ? server_.arg("ms").toInt() : TEST_LED_DURATION_MS;
    if (ms > 60000) { sendError(400, "ms must be <= 60000"); return; }
    cancelEspNowTest();
    if (server_.arg("hold") == "1") {
        // Manual on/off at the saved brightness level: stays until the next scheduled switch
        activeTest_ = 0;
        bool on = server_.arg("state") == "on";
        hardware_->setLEDBrightness(on ? hardware_->getLedOnPwm() : 0);
        sendOk("\"state\":\"" + String(on ? "on" : "off") + "\",\"hold\":true");
        return;
    }
    hardware_->setLEDBrightness((uint8_t)level);
    activeTest_ = 2;
    testStartMs_ = millis();
    testDurationMs_ = ms;
    sendOk("\"level\":" + String(level) + ",\"ms\":" + String(ms));
}

// ============================================================================
// Sensors
// ============================================================================

void TestServer::handleSensors() {
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    float moisture = hardware_->readMoistureSensor();
    bool ir = hardware_->readIRSensor();
    String fields = "\"moisture\":" + String(moisture, 1) +
                    ",\"irDetected\":" + String(ir ? "true" : "false");
    sendOk(fields);
}

// ============================================================================
// Settings (pump duration/cooldown, auto-dispense, LED night schedule)
// ============================================================================

void TestServer::handleGetSettings() {
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    String fields = "\"pumpDurationMs\":" + String(hardware_->getMaxDispenseDurationMs()) +
                    ",\"pumpCooldownMs\":" + String(hardware_->getDispenseCooldownMs()) +
                    ",\"autoDispense\":" + String((autoDispenseEnabled_ && *autoDispenseEnabled_) ? "true" : "false") +
                    ",\"ledLevel\":" + String(hardware_->getLedLevel()) +
                    ",\"ledSchedule\":" + String(hardware_->isLedScheduleEnabled() ? "true" : "false") +
                    ",\"ledOn\":\"" + HardwareAbstraction::formatMinute(hardware_->getLedOnMinute()) + "\"" +
                    ",\"ledOff\":\"" + HardwareAbstraction::formatMinute(hardware_->getLedOffMinute()) + "\"";
    sendOk(fields);
}

void TestServer::handleSetPumpDuration() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (!server_.hasArg("ms") || server_.arg("ms").toInt() <= 0) { sendError(400, "ms must be a positive integer"); return; }
    long ms = server_.arg("ms").toInt();
    if (ms > 25500) { sendError(400, "ms must be <= 25500"); return; }
    hardware_->setMaxDispenseDurationMs((unsigned long)ms);
    sendOk("\"pumpDurationMs\":" + String(ms));
}

void TestServer::handleSetPumpCooldown() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (!server_.hasArg("ms") || server_.arg("ms").toInt() <= 0) { sendError(400, "ms must be a positive integer"); return; }
    long ms = server_.arg("ms").toInt();
    if (ms > 25500) { sendError(400, "ms must be <= 25500"); return; }
    hardware_->setDispenseCooldownMs((unsigned long)ms);
    sendOk("\"pumpCooldownMs\":" + String(ms));
}

void TestServer::handleSetAutoDispense() {
    if (!isAuthorized()) return;
    if (!autoDispenseEnabled_) { sendError(503, "auto-dispense state not available"); return; }
    if (!server_.hasArg("enabled")) { sendError(400, "enabled must be 0 or 1"); return; }
    *autoDispenseEnabled_ = server_.arg("enabled").toInt() != 0;
    sendOk("\"autoDispense\":" + String(*autoDispenseEnabled_ ? "true" : "false"));
}

void TestServer::handleSetLedSchedule() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (!server_.hasArg("enabled")) { sendError(400, "enabled must be 0 or 1"); return; }
    bool on = server_.arg("enabled").toInt() != 0;
    hardware_->setLedScheduleEnabled(on);
    sendOk("\"ledSchedule\":" + String(on ? "true" : "false"));
}

// "HH:MM" (24h) -> minutes since midnight, or -1 if malformed
static int parseHhMm(const String& v) {
    int colon = v.indexOf(':');
    if (colon < 1) return -1;
    int h = v.substring(0, colon).toInt(), m = v.substring(colon + 1).toInt();
    if (h < 0 || h > 23 || m < 0 || m > 59) return -1;
    return h * 60 + m;
}

void TestServer::handleSetLedLevel() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    int pct = server_.arg("pct").toInt();
    if (pct < 1 || pct > 100) { sendError(400, "pct must be 1-100"); return; }
    hardware_->setLedLevel((uint8_t)pct);
    sendOk("\"ledLevel\":" + String(pct));
}

void TestServer::handleSetLedScheduleTimes() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    int on = parseHhMm(server_.arg("on")), off = parseHhMm(server_.arg("off"));
    if (on < 0 || off < 0) { sendError(400, "on and off must be HH:MM (24h)"); return; }
    if (!hardware_->setLedScheduleTimes((uint16_t)on, (uint16_t)off)) { sendError(400, "on and off must differ"); return; }
    sendOk("\"ledOn\":\"" + HardwareAbstraction::formatMinute(on) + "\",\"ledOff\":\"" +
           HardwareAbstraction::formatMinute(off) + "\"");
}

// ============================================================================
// Status / root
// ============================================================================

void TestServer::handleStatus() {
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    server_.send(200, "application/json", hardware_->getStatusJSON());
}

static const char* resetReasonName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "software restart";
        case ESP_RST_PANIC:    return "crash (panic)";
        case ESP_RST_INT_WDT:  return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT:      return "watchdog";
        case ESP_RST_BROWNOUT: return "brownout (power dip)";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_EXT:      return "reset pin";
        default:               return "unknown";
    }
}

// Seconds since a millis() timestamp, or -1 when it never happened
static long agoSeconds(unsigned long ms) { return ms ? (long)((millis() - ms) / 1000) : -1; }

void TestServer::handleSystem() {
    String fields = "\"firmware\":\"" FIRMWARE_VERSION "\"" +
                    String(",\"build\":\"" __DATE__ " " __TIME__ "\"") +
                    ",\"uptimeS\":" + String(millis() / 1000) +
                    ",\"resetReason\":\"" + resetReasonName(esp_reset_reason()) + "\"" +
                    ",\"freeHeap\":" + String(ESP.getFreeHeap()) +
                    ",\"minFreeHeap\":" + String(ESP.getMinFreeHeap()) +
                    ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false") +
                    ",\"rssi\":" + String(WiFi.RSSI()) +
                    ",\"channel\":" + String(WiFi.channel()) +
                    ",\"ip\":\"" + WiFi.localIP().toString() + "\"" +
                    ",\"mac\":\"" + WiFi.macAddress() + "\"";
    if (espNow_) {
        EspNowService::LinkStats s = espNow_->getLinkStats();
        fields += ",\"espNow\":{\"initialized\":" + String(espNow_->isInitialized() ? "true" : "false") +
                  ",\"sentOk\":" + String(s.sentOk) + ",\"sentFail\":" + String(s.sentFail) +
                  ",\"lastSendOkAgoS\":" + String(agoSeconds(s.lastSendOkMs)) +
                  ",\"commandsReceived\":" + String(s.commandsReceived) +
                  ",\"lastCommandAgoS\":" + String(agoSeconds(s.lastCommandMs)) + "}";
    }
    sendOk(fields);
}

void TestServer::update() {
    if (activeTest_ == 0 || !hardware_) return;
    unsigned long elapsed = millis() - testStartMs_;
    if (elapsed < testDurationMs_) return;
    if (activeTest_ == 1) hardware_->stopPump();
    else if (activeTest_ == 2) {
        hardware_->setLEDBrightness(0);
        hardware_->reapplyLEDSchedule();  // restore scheduled state after the test pulse
    }
    activeTest_ = 0;
}

void TestServer::handleRoot() {
    static const char PAGE[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Prick'n'Print Bench</title>
<meta name="theme-color" content="#1C2A1F">
<style>
)HTMLPAGE";
    static const char PAGE_BODY[] PROGMEM = R"HTMLPAGE(
  /* Page layout only - colours and components come from WebTheme.h */
  .page { max-width: 820px; display: flex; flex-direction: column; gap: 14px; }
  .card h2 { display: flex; align-items: center; gap: 10px; }
  .card h2 .hint { margin-left: auto; font-size: 0.75rem; font-weight: 500; color: var(--text-faint); }
  .row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
  .row + .row { margin-top: 10px; }
  .row label { color: var(--text-muted); font-size: 0.85rem; }
  .split { display: grid; grid-template-columns: 1fr 1fr; gap: 14px; }
  @media (max-width: 640px) { .split { grid-template-columns: 1fr; } }
  .tile-grid { grid-template-columns: repeat(auto-fill, minmax(130px, 1fr)); }
  .tile .value { font-family: var(--font-mono); font-size: 1.1rem; }
  input[type=number] { width: 96px; font-family: var(--font-mono); }
  input[type=range] { flex: 1; min-width: 140px; }
  .kv { display: grid; grid-template-columns: max-content 1fr; gap: 6px 16px; font-size: 0.85rem; }
  .kv dt { color: var(--text-faint); }
  .kv dd { font-family: var(--font-mono); color: var(--text); overflow-wrap: anywhere; }
  .kv dd.ok { color: var(--accent-light); }
  .kv dd.warn { color: var(--warn); }
  .kv dd.bad { color: var(--danger); }
  .setting { display: flex; align-items: center; gap: 10px; min-height: 44px; }
  .setting + .setting { border-top: 1px solid var(--border); }
  .setting .name { flex: 1; }
  .setting .name small { display: block; color: var(--text-faint); font-size: 0.75rem; }
  details.card > summary { cursor: pointer; list-style: none; font-weight: 600; display: flex; align-items: center; gap: 10px; }
  details.card > summary::-webkit-details-marker { display: none; }
  details.card[open] > summary { margin-bottom: 12px; }
  #keyState { font-weight: 500; font-size: 0.8rem; }
  #keyState.ok { color: var(--accent-light); }
  #keyState.bad { color: var(--warn); }
  #log {
    background: var(--surface-2); border-radius: var(--radius-sm);
    padding: 10px 12px; font-family: var(--font-mono); font-size: 0.78rem;
    max-height: 240px; overflow-y: auto; white-space: pre-wrap; color: var(--text-muted);
  }
  #log .err { color: var(--danger); }
  #log .entry { border-bottom: 1px solid var(--border); padding: 4px 0; }
  #log .entry:last-child { border-bottom: none; }
  #log:empty::before { content: "Run a test to see its result here."; color: var(--text-faint); font-family: var(--font-ui); }
</style>
</head>
<body>
<header class="topbar">
  <div>
    <h1>Bench test</h1>
    <div class="sub">Main ESP32 &middot; hardware &amp; software</div>
  </div>
  <span class="spacer"></span>
  <span class="status-pill" id="statusPill"><span class="dot"></span><span id="statusLabel">connecting</span></span>
</header>
<div class="page">
  <details class="card" id="keyCard">
    <summary>Access key <span id="keyState"></span></summary>
    <div class="row">
      <input type="password" id="apiKey" placeholder="WEB_PASSWORD from secrets.h" autocomplete="off" style="flex:1">
      <button class="btn btn-primary" onclick="saveKey()">Save</button>
    </div>
    <p class="note" style="margin-top:8px;color:var(--text-faint);font-size:0.8rem">Needed for every test and setting. Reading status works without it.</p>
  </details>

  <section class="card">
    <h2>Hardware status <span class="hint">every 2 s</span></h2>
    <div class="tile-grid">
      <div class="tile"><div class="label">Printer</div><div class="value" id="s-printer">&hellip;</div></div>
      <div class="tile"><div class="label">Pump</div><div class="value" id="s-pump">&hellip;</div></div>
      <div class="tile"><div class="label">Dispensing</div><div class="value" id="s-dispensing">&hellip;</div></div>
      <div class="tile"><div class="label">IR sensor</div><div class="value" id="s-ir">&hellip;</div></div>
      <div class="tile"><div class="label">Moisture</div><div class="value" id="s-moisture">&hellip;</div></div>
      <div class="tile"><div class="label">Sanitizer</div><div class="value" id="s-sanitizer">&hellip;</div></div>
      <div class="tile"><div class="label">LED</div><div class="value" id="s-led">&hellip;</div></div>
      <div class="tile"><div class="label">Dispenses</div><div class="value" id="s-dispenses">&hellip;</div></div>
    </div>
  </section>

  <section class="card">
    <h2>System <span class="hint">every 5 s</span></h2>
    <div class="split">
      <dl class="kv">
        <dt>Firmware</dt><dd id="y-fw">&hellip;</dd>
        <dt>Built</dt><dd id="y-build">&hellip;</dd>
        <dt>Uptime</dt><dd id="y-up">&hellip;</dd>
        <dt>Last reset</dt><dd id="y-reset">&hellip;</dd>
        <dt>Free heap</dt><dd id="y-heap">&hellip;</dd>
      </dl>
      <dl class="kv">
        <dt>WiFi</dt><dd id="y-wifi">&hellip;</dd>
        <dt>IP / MAC</dt><dd id="y-ip">&hellip;</dd>
        <dt>To display</dt><dd id="y-tx">&hellip;</dd>
        <dt>From display</dt><dd id="y-rx">&hellip;</dd>
      </dl>
    </div>
  </section>

  <div class="split">
    <section class="card">
      <h2>Printer</h2>
      <div class="row">
        <button class="btn btn-primary" onclick="run('/printer/test')">Test pattern</button>
        <button class="btn" onclick="run('/printer/raw')">Raw line</button>
        <button class="btn" onclick="run('/printer/receipt')">Receipt</button>
      </div>
      <div class="row">
        <label for="feedLines">Feed</label>
        <input type="number" id="feedLines" value="3" min="1" max="50">
        <button class="btn" onclick="run('/printer/feed?lines=' + val('feedLines'))">Feed lines</button>
      </div>
    </section>

    <section class="card">
      <h2>Pump &amp; sensors</h2>
      <div class="row">
        <label for="pumpMs">Run for</label>
        <input type="number" id="pumpMs" value="800" min="1" max="5000">
        <label>ms</label>
        <button class="btn btn-primary" onclick="run('/pump?ms=' + val('pumpMs'))">Run pump</button>
      </div>
      <div class="row">
        <button class="btn" onclick="run('/sensors')">Read moisture + IR now</button>
      </div>
    </section>
  </div>

  <section class="card">
    <h2>LED test</h2>
    <div class="row">
      <label for="ledLevel">PWM</label>
      <input type="number" id="ledLevel" value="255" min="0" max="255">
      <label for="ledMs">for</label>
      <input type="number" id="ledMs" value="1000" min="1" max="60000">
      <label>ms</label>
      <button class="btn btn-primary" onclick="run('/led?level=' + val('ledLevel') + '&ms=' + val('ledMs'))">Pulse</button>
      <span class="spacer" style="flex:1"></span>
      <button class="btn" onclick="run('/led?hold=1&state=on')">Hold on</button>
      <button class="btn btn-danger" onclick="run('/led?hold=1&state=off')">Hold off</button>
    </div>
  </section>

  <section class="card">
    <h2>Settings <span class="hint">saved on the board</span></h2>
    <div class="setting">
      <div class="name">Pump duration<small>longest single dispense</small></div>
      <input type="number" id="setDurMs" min="1" max="25500"><label>ms</label>
      <button class="btn" onclick="run('/settings/pump-duration?ms=' + val('setDurMs'), loadSettings)">Set</button>
    </div>
    <div class="setting">
      <div class="name">Pump cooldown<small>wait between dispenses</small></div>
      <input type="number" id="setCoolMs" min="1" max="25500"><label>ms</label>
      <button class="btn" onclick="run('/settings/pump-cooldown?ms=' + val('setCoolMs'), loadSettings)">Set</button>
    </div>
    <div class="setting">
      <div class="name">Auto-dispense<small>IR sensor triggers the pump</small></div>
      <label class="switch"><input type="checkbox" id="setAuto" onchange="run('/settings/auto-dispense?enabled=' + (this.checked ? 1 : 0), loadSettings)"><span></span></label>
    </div>
    <div class="setting">
      <div class="name">LED brightness<small>level used when the LED is on</small></div>
      <input type="range" id="ledLevelPct" min="1" max="100" value="100"
             oninput="el('ledLevelVal').textContent = this.value + '%'"
             onchange="run('/settings/led-level?pct=' + this.value, loadSettings)">
      <span id="ledLevelVal" style="min-width:40px;text-align:right;font-family:var(--font-mono)">&hellip;</span>
    </div>
    <div class="setting">
      <div class="name">LED night schedule<small>turns the LED on and off by clock</small></div>
      <label class="switch"><input type="checkbox" id="setSched" onchange="run('/settings/led-schedule?enabled=' + (this.checked ? 1 : 0), loadSettings)"><span></span></label>
    </div>
    <div class="setting">
      <div class="name">Schedule times</div>
      <input type="time" id="ledOnTime"><label>to</label><input type="time" id="ledOffTime">
      <button class="btn" onclick="run('/settings/led-schedule-times?on=' + val('ledOnTime') + '&off=' + val('ledOffTime'), loadSettings)">Set</button>
    </div>
  </section>

  <section class="card">
    <h2>Activity log</h2>
    <div id="log"></div>
  </section>
</div>

<script>
function el(id) { return document.getElementById(id); }
function val(id) { return el(id).value; }

function log(text, isErr) {
  const entry = document.createElement('div');
  entry.className = 'entry' + (isErr ? ' err' : '');
  entry.textContent = '[' + new Date().toLocaleTimeString() + '] ' + text;
  el('log').insertBefore(entry, el('log').firstChild);
  while (el('log').children.length > 40) el('log').removeChild(el('log').lastChild);
}

function getKey() { try { return localStorage.getItem('testServerKey') || ''; } catch (e) { return ''; } }
function showKeyState() {
  const has = !!getKey();
  el('keyState').textContent = has ? 'saved' : 'not set';
  el('keyState').className = has ? 'ok' : 'bad';
  el('keyCard').open = !has;
}
function saveKey() {
  try { localStorage.setItem('testServerKey', val('apiKey')); } catch (e) {}
  log('Key saved.');
  showKeyState();
}

function run(path, after) {
  fetch(path, { headers: { 'X-API-Key': getKey() } })
    .then(function(r) { return r.text().then(function(t) { return {ok: r.ok, status: r.status, text: t}; }); })
    .then(function(res) {
      log(path + ' -> ' + res.text.trim(), !res.ok);
      if (res.status === 401) { el('keyCard').open = true; log('Wrong or missing access key.', true); }
      refreshStatus();
      if (after) after();
    })
    .catch(function(e) { log(path + ' -> ERROR: ' + e, true); });
}

function setVal(id, text, cls) { el(id).textContent = text; el(id).className = 'value' + (cls ? ' ' + cls : ''); }
function setKv(id, text, cls) { el(id).textContent = text; el(id).className = cls || ''; }

function setConn(ok) {
  el('statusPill').className = 'status-pill ' + (ok ? 'ok' : 'bad');
  el('statusLabel').textContent = ok ? 'online' : 'unreachable';
}

function refreshStatus() {
  fetch('/status').then(function(r) { return r.json(); }).then(function(d) {
    setConn(true);
    setVal('s-printer', d.printer ? 'Ready' : 'Not ready', d.printer ? 'on' : 'bad');
    setVal('s-pump', d.pump ? 'Running' : 'Stopped', d.pump ? 'on' : 'off');
    setVal('s-dispensing', d.dispensing ? 'Yes' : 'No', d.dispensing ? 'on' : 'off');
    setVal('s-ir', d.irSensor ? 'Detected' : 'Clear', d.irSensor ? 'on' : 'off');
    setVal('s-moisture', d.moisture + '%');
    setVal('s-sanitizer', d.sanitizer + '%', parseFloat(d.sanitizer) < 20 ? 'bad' : '');
    setVal('s-led', d.ledBrightness > 0 ? 'On · ' + d.ledLevel + '%' : 'Off', d.ledBrightness > 0 ? 'on' : 'off');
    setVal('s-dispenses', d.dispenses);
  }).catch(function() { setConn(false); });
}

function dur(s) {
  if (s < 0) return 'never';
  if (s < 60) return s + ' s';
  if (s < 3600) return Math.floor(s / 60) + ' min';
  if (s < 86400) return Math.floor(s / 3600) + ' h ' + Math.floor(s % 3600 / 60) + ' min';
  return Math.floor(s / 86400) + ' d ' + Math.floor(s % 86400 / 3600) + ' h';
}
function refreshSystem() {
  fetch('/system').then(function(r) { return r.json(); }).then(function(d) {
    setKv('y-fw', 'v' + d.firmware);
    setKv('y-build', d.build);
    setKv('y-up', dur(d.uptimeS));
    const badReset = /crash|watchdog|brownout/.test(d.resetReason);
    setKv('y-reset', d.resetReason, badReset ? 'bad' : '');
    setKv('y-heap', Math.round(d.freeHeap / 1024) + ' KB (low ' + Math.round(d.minFreeHeap / 1024) + ' KB)', d.minFreeHeap < 20000 ? 'warn' : '');
    setKv('y-wifi', d.wifiConnected ? d.rssi + ' dBm · ch ' + d.channel : 'disconnected',
          !d.wifiConnected ? 'bad' : d.rssi < -75 ? 'warn' : 'ok');
    setKv('y-ip', d.ip + ' · ' + d.mac);
    const n = d.espNow;
    if (!n) { setKv('y-tx', 'n/a'); setKv('y-rx', 'n/a'); return; }
    if (!n.initialized) { setKv('y-tx', 'ESP-NOW not running', 'bad'); setKv('y-rx', '—'); return; }
    const total = n.sentOk + n.sentFail, okPct = total ? Math.round(100 * n.sentOk / total) : 0;
    const fresh = n.lastSendOkAgoS >= 0 && n.lastSendOkAgoS < 30;
    setKv('y-tx', total ? okPct + '% delivered (' + n.sentFail + ' failed) · last ok ' + dur(n.lastSendOkAgoS) + ' ago'
                        : 'nothing sent yet', fresh ? 'ok' : 'bad');
    setKv('y-rx', n.commandsReceived + ' commands · last ' + (n.lastCommandAgoS < 0 ? 'never' : dur(n.lastCommandAgoS) + ' ago'));
  }).catch(function() {});
}

function hhmm(t) {  // "4:45 PM" -> "16:45" for <input type=time>
  const m = /(\d+):(\d+)\s*(AM|PM)/i.exec(t || '');
  if (!m) return '';
  let h = parseInt(m[1], 10) % 12;
  if (m[3].toUpperCase() === 'PM') h += 12;
  return String(h).padStart(2, '0') + ':' + m[2];
}
function loadSettings() {
  fetch('/settings').then(function(r) { return r.json(); }).then(function(d) {
    el('setDurMs').value = d.pumpDurationMs;
    el('setCoolMs').value = d.pumpCooldownMs;
    el('setAuto').checked = !!d.autoDispense;
    el('setSched').checked = !!d.ledSchedule;
    el('ledOnTime').value = hhmm(d.ledOn);
    el('ledOffTime').value = hhmm(d.ledOff);
    if (document.activeElement !== el('ledLevelPct')) {
      el('ledLevelPct').value = d.ledLevel;
      el('ledLevelVal').textContent = d.ledLevel + '%';
    }
  }).catch(function() {});
}

el('apiKey').value = getKey();
showKeyState();
refreshStatus();
refreshSystem();
loadSettings();
setInterval(refreshStatus, 2000);
setInterval(refreshSystem, 5000);
</script>
</body>
</html>
)HTMLPAGE";
    sendHtmlParts(server_, {PAGE, WEB_THEME_CSS, PAGE_BODY});
}

void TestServer::begin() {
    // WebServer only retains headers explicitly registered here; without this,
    // server_.header("X-API-Key") always returns empty regardless of what the client sends.
    static const char* headerKeys[] = {"X-API-Key"};
    server_.collectHeaders(headerKeys, 1);

    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/status", HTTP_GET, [this]() { handleStatus(); });
    server_.on("/system", HTTP_GET, [this]() { handleSystem(); });
    server_.on("/printer/test", HTTP_GET, [this]() { handlePrinterTest(); });
    server_.on("/printer/raw", HTTP_GET, [this]() { handlePrinterRaw(); });
    server_.on("/printer/receipt", HTTP_GET, [this]() { handlePrinterReceipt(); });
    server_.on("/printer/feed", HTTP_GET, [this]() { handlePrinterFeed(); });
    server_.on("/pump", HTTP_GET, [this]() { handlePump(); });
    server_.on("/led", HTTP_GET, [this]() { handleLed(); });
    server_.on("/sensors", HTTP_GET, [this]() { handleSensors(); });
    server_.on("/settings", HTTP_GET, [this]() { handleGetSettings(); });
    server_.on("/settings/pump-duration", HTTP_GET, [this]() { handleSetPumpDuration(); });
    server_.on("/settings/pump-cooldown", HTTP_GET, [this]() { handleSetPumpCooldown(); });
    server_.on("/settings/auto-dispense", HTTP_GET, [this]() { handleSetAutoDispense(); });
    server_.on("/settings/led-schedule", HTTP_GET, [this]() { handleSetLedSchedule(); });
    server_.on("/settings/led-schedule-times", HTTP_GET, [this]() { handleSetLedScheduleTimes(); });
    server_.on("/settings/led-level", HTTP_GET, [this]() { handleSetLedLevel(); });
    server_.begin();
    Logger::info("Main", "Hardware test server on port 8081 (auth: X-API-Key header or ?key=, matches WEB_PASSWORD)");
}
