#include "TestServer.h"
#include "Logger.h"
#include "secrets.h"

static const unsigned long TEST_PUMP_DURATION_MS = 800;
static const unsigned long TEST_LED_DURATION_MS = 1000;

// Defined in main.cpp: cancels the ESP-NOW-driven test-pulse timer so it
// doesn't also try to shut off the actuator this server just started testing.
void cancelEspNowTest();

TestServer::TestServer(HardwareAbstraction* hw, PrinterService* printer, bool* autoDispense)
    : server_(8081), hardware_(hw), printerService_(printer), autoDispenseEnabled_(autoDispense) {
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
    hardware_->setAutoBrightness(false);
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
    float light = hardware_->readLightSensor();
    String fields = "\"moisture\":" + String(moisture, 1) +
                    ",\"irDetected\":" + String(ir ? "true" : "false") +
                    ",\"light\":" + String(light, 1);
    sendOk(fields);
}

// ============================================================================
// Settings (pump duration/cooldown, auto-dispense, auto-brightness, light calibration)
// ============================================================================

void TestServer::handleGetSettings() {
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    String fields = "\"pumpDurationMs\":" + String(hardware_->getMaxDispenseDurationMs()) +
                    ",\"pumpCooldownMs\":" + String(hardware_->getDispenseCooldownMs()) +
                    ",\"autoDispense\":" + String((autoDispenseEnabled_ && *autoDispenseEnabled_) ? "true" : "false") +
                    ",\"autoBrightness\":" + String(hardware_->isAutoBrightnessEnabled() ? "true" : "false");
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

void TestServer::handleSetAutoBrightness() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (!server_.hasArg("enabled")) { sendError(400, "enabled must be 0 or 1"); return; }
    bool on = server_.arg("enabled").toInt() != 0;
    hardware_->setAutoBrightness(on);
    sendOk("\"autoBrightness\":" + String(on ? "true" : "false"));
}

void TestServer::handleSetLightCalibration() {
    if (!isAuthorized()) return;
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    if (!server_.hasArg("point") || (server_.arg("point") != "min" && server_.arg("point") != "max")) {
        sendError(400, "point must be 'min' or 'max'"); return;
    }
    hardware_->readLightSensor();
    float pct = hardware_->getLightPercent();
    if (server_.arg("point") == "max") hardware_->setLightCalibrationMax(pct);
    else hardware_->setLightCalibrationMin(pct);
    if (hardware_->isAutoBrightnessEnabled()) hardware_->updateLEDBrightness();
    sendOk("\"point\":\"" + server_.arg("point") + "\",\"light\":" + String(pct, 1));
}

// ============================================================================
// Status / root
// ============================================================================

void TestServer::handleStatus() {
    if (!hardware_) { sendError(503, "hardware not initialized"); return; }
    server_.send(200, "application/json", hardware_->getStatusJSON());
}

void TestServer::update() {
    if (activeTest_ == 0 || !hardware_) return;
    unsigned long elapsed = millis() - testStartMs_;
    if (elapsed < testDurationMs_) return;
    if (activeTest_ == 1) hardware_->stopPump();
    else if (activeTest_ == 2) hardware_->setLEDBrightness(0);
    activeTest_ = 0;
}

void TestServer::handleRoot() {
    static const char PAGE[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>Print-n-Prick — Bench Test</title>
<style>
  :root {
    --bg: #0b0d10; --surface: #14171c; --surface-2: #1b1f26; --surface-hover: #20242c;
    --border: #262b33; --border-strong: #333a45;
    --text: #e7e9ec; --text-muted: #8b93a1; --text-faint: #565e6b;
    --accent: #4f8cff; --accent-hover: #6b9dff; --accent-dim: rgba(79, 140, 255, 0.14);
    --ok: #3ddc84; --ok-dim: rgba(61, 220, 132, 0.14);
    --danger: #ff5470; --danger-hover: #ff7089; --danger-dim: rgba(255, 84, 112, 0.14);
    --font-ui: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    --font-mono: "JetBrains Mono", "SF Mono", ui-monospace, Consolas, monospace;
    color-scheme: dark;
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; padding: 24px; min-height: 100vh;
    background: var(--bg); color: var(--text);
    font-family: var(--font-ui);
    -webkit-font-smoothing: antialiased;
  }
  .wrap { max-width: 760px; margin: 0 auto; }
  header {
    display: flex; align-items: baseline; justify-content: space-between; gap: 12px; flex-wrap: wrap;
    padding-bottom: 16px; margin-bottom: 24px; border-bottom: 1px solid var(--border);
  }
  h1 { font-size: 1.2rem; font-weight: 700; letter-spacing: -0.01em; margin: 0; }
  .sub { color: var(--text-muted); font-size: 0.8rem; margin-top: 2px; }
  .status-pill {
    display: inline-flex; align-items: center; gap: 6px; padding: 5px 11px; border-radius: 999px;
    background: var(--surface-2); border: 1px solid var(--border);
    font-size: 0.75rem; font-family: var(--font-mono); color: var(--text-muted);
  }
  .status-pill .dot { width: 6px; height: 6px; border-radius: 50%; background: var(--text-faint); }
  .status-pill.ok { color: var(--ok); border-color: color-mix(in srgb, var(--ok) 35%, var(--border)); }
  .status-pill.ok .dot { background: var(--ok); box-shadow: 0 0 0 3px var(--ok-dim); }
  .status-pill.bad { color: var(--danger); border-color: color-mix(in srgb, var(--danger) 35%, var(--border)); }
  .status-pill.bad .dot { background: var(--danger); box-shadow: 0 0 0 3px var(--danger-dim); }

  .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(120px, 1fr)); gap: 10px; margin-bottom: 28px; }
  .stat { background: var(--surface); border: 1px solid var(--border); border-radius: 10px; padding: 12px 14px; }
  .stat .label { font-size: 0.7rem; color: var(--text-faint); text-transform: uppercase; letter-spacing: 0.05em; }
  .stat .value { font-family: var(--font-mono); font-variant-numeric: tabular-nums; font-size: 1.35rem; font-weight: 600; margin-top: 3px; color: var(--text); }
  .stat .value.on { color: var(--ok); }
  .stat .value.off { color: var(--text-faint); }

  section { margin-bottom: 26px; }
  section h2 {
    font-size: 0.72rem; text-transform: uppercase; letter-spacing: 0.07em;
    color: var(--text-muted); margin: 0 0 12px; font-weight: 600;
  }
  .row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
  .row + .row { margin-top: 10px; }

  button {
    background: var(--surface-2); color: var(--text); border: 1px solid var(--border);
    border-radius: 7px; padding: 9px 14px; font-size: 0.85rem; font-family: inherit; font-weight: 500;
    cursor: pointer; transition: background 0.15s, border-color 0.15s;
  }
  button:hover { background: var(--surface-hover); border-color: var(--border-strong); }
  button:active { transform: scale(0.98); }
  button:focus-visible { outline: 2px solid var(--accent); outline-offset: 2px; }
  button.primary { background: var(--accent); color: #fff; border-color: var(--accent); }
  button.primary:hover { background: var(--accent-hover); }
  button.danger { background: transparent; color: var(--danger); border-color: color-mix(in srgb, var(--danger) 40%, var(--border)); }
  button.danger:hover { background: var(--danger-dim); border-color: var(--danger); }

  input[type=number], input[type=text] {
    width: 90px; background: var(--surface); border: 1px solid var(--border); color: var(--text);
    border-radius: 7px; padding: 8px 10px; font-size: 0.85rem; font-family: var(--font-mono);
  }
  input[type=text] { width: 180px; font-family: inherit; }
  input:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 2px var(--accent-dim); }
  label { font-size: 0.8rem; color: var(--text-muted); }

  #log {
    background: #07080a; border: 1px solid var(--border); border-radius: 10px;
    padding: 12px 14px; font-family: var(--font-mono); font-size: 0.78rem;
    max-height: 240px; overflow-y: auto; white-space: pre-wrap; color: var(--ok);
  }
  #log .err { color: var(--danger); }
  #log .entry { border-bottom: 1px solid var(--surface-2); padding: 4px 0; }
  #log .entry:last-child { border-bottom: none; }
  #log:empty::before { content: "No activity yet."; color: var(--text-faint); font-family: var(--font-ui); }
</style>
</head>
<body>
<div class="wrap">
  <header>
    <div>
      <h1>Print-n-Prick — Bench Test</h1>
      <div class="sub">Main ESP32 diagnostics: printer, pump, LED, sensors.</div>
    </div>
    <span class="status-pill" id="statusPill"><span class="dot"></span><span id="statusLabel">connecting</span></span>
  </header>

  <section>
    <h2>Access key</h2>
    <div class="row">
      <label for="apiKey">Key (matches WEB_PASSWORD)</label>
      <input type="text" id="apiKey" placeholder="key">
      <button onclick="saveKey()">Save</button>
    </div>
  </section>

  <section>
    <h2>Status</h2>
    <div class="grid" id="statusGrid">
      <div class="stat"><div class="label">Printer</div><div class="value" id="s-printer">&hellip;</div></div>
      <div class="stat"><div class="label">Pump</div><div class="value" id="s-pump">&hellip;</div></div>
      <div class="stat"><div class="label">Dispensing</div><div class="value" id="s-dispensing">&hellip;</div></div>
      <div class="stat"><div class="label">IR Sensor</div><div class="value" id="s-ir">&hellip;</div></div>
      <div class="stat"><div class="label">Moisture</div><div class="value" id="s-moisture">&hellip;</div></div>
      <div class="stat"><div class="label">Light</div><div class="value" id="s-light">&hellip;</div></div>
      <div class="stat"><div class="label">Sanitizer</div><div class="value" id="s-sanitizer">&hellip;</div></div>
      <div class="stat"><div class="label">LED</div><div class="value" id="s-led">&hellip;</div></div>
      <div class="stat"><div class="label">Dispenses</div><div class="value" id="s-dispenses">&hellip;</div></div>
    </div>
  </section>

  <section>
    <h2>Printer</h2>
    <div class="row">
      <button class="primary" onclick="run('/printer/test')">Test pattern</button>
      <button onclick="run('/printer/raw')">Raw line</button>
      <button onclick="run('/printer/receipt')">Sample receipt</button>
    </div>
    <div class="row">
      <label for="feedLines">Feed lines</label>
      <input type="number" id="feedLines" value="3" min="1" max="50">
      <button onclick="run('/printer/feed?lines=' + val('feedLines'))">Feed</button>
    </div>
  </section>

  <section>
    <h2>Pump</h2>
    <div class="row">
      <label for="pumpMs">Duration (ms)</label>
      <input type="number" id="pumpMs" value="800" min="1" max="5000">
      <button class="primary" onclick="run('/pump?ms=' + val('pumpMs'))">Run pump</button>
    </div>
  </section>

  <section>
    <h2>LED</h2>
    <div class="row">
      <label for="ledLevel">Level (0–255)</label>
      <input type="number" id="ledLevel" value="255" min="0" max="255">
      <label for="ledMs">Duration (ms)</label>
      <input type="number" id="ledMs" value="1000" min="1" max="60000">
      <button class="primary" onclick="run('/led?level=' + val('ledLevel') + '&ms=' + val('ledMs'))">Run LED</button>
      <button class="danger" onclick="run('/led?level=0&ms=1')">Off</button>
    </div>
  </section>

  <section>
    <h2>Sensors</h2>
    <div class="row">
      <button onclick="run('/sensors')">Read now</button>
    </div>
  </section>

  <section>
    <h2>Settings</h2>
    <div class="row">
      <label for="setDurMs">Pump duration (ms)</label>
      <input type="number" id="setDurMs" min="1" max="25500" placeholder="2000">
      <button onclick="run('/settings/pump-duration?ms=' + val('setDurMs'))">Set</button>
    </div>
    <div class="row">
      <label for="setCoolMs">Pump cooldown (ms)</label>
      <input type="number" id="setCoolMs" min="1" max="25500" placeholder="3000">
      <button onclick="run('/settings/pump-cooldown?ms=' + val('setCoolMs'))">Set</button>
    </div>
    <div class="row">
      <label>Auto-dispense (IR triggers pump)</label>
      <button onclick="run('/settings/auto-dispense?enabled=1')">On</button>
      <button onclick="run('/settings/auto-dispense?enabled=0')">Off</button>
    </div>
    <div class="row">
      <label>Auto-brightness (LED follows light sensor)</label>
      <button onclick="run('/settings/auto-brightness?enabled=1')">On</button>
      <button onclick="run('/settings/auto-brightness?enabled=0')">Off</button>
    </div>
    <div class="row">
      <label>Light calibration</label>
      <button onclick="run('/settings/light-calibration?point=min')">Record dark (LED off)</button>
      <button onclick="run('/settings/light-calibration?point=max')">Record bright (LED on)</button>
    </div>
    <div class="row">
      <button onclick="run('/settings')">Refresh settings</button>
    </div>
  </section>

  <section>
    <h2>Activity log</h2>
    <div id="log"></div>
  </section>
</div>

<script>
function val(id) { return document.getElementById(id).value; }

function log(text, isErr) {
  const el = document.getElementById('log');
  const entry = document.createElement('div');
  entry.className = 'entry' + (isErr ? ' err' : '');
  const time = new Date().toLocaleTimeString();
  entry.textContent = '[' + time + '] ' + text;
  el.insertBefore(entry, el.firstChild);
  while (el.children.length > 40) el.removeChild(el.lastChild);
}

function getKey() { return localStorage.getItem('testServerKey') || ''; }
function saveKey() {
  localStorage.setItem('testServerKey', val('apiKey'));
  log('Key saved.');
}

function run(path) {
  fetch(path, { headers: { 'X-API-Key': getKey() } })
    .then(function(r) { return r.text().then(function(t) { return {ok: r.ok, text: t}; }); })
    .then(function(res) { log(path + ' -> ' + res.text.trim(), !res.ok); refreshStatus(); })
    .catch(function(e) { log(path + ' -> ERROR: ' + e, true); });
}

function setVal(id, text, cls) {
  const el = document.getElementById(id);
  el.textContent = text;
  el.className = 'value' + (cls ? ' ' + cls : '');
}

function setConn(ok) {
  const pill = document.getElementById('statusPill');
  const label = document.getElementById('statusLabel');
  pill.className = 'status-pill ' + (ok ? 'ok' : 'bad');
  label.textContent = ok ? 'online' : 'unreachable';
}

function refreshStatus() {
  fetch('/status').then(function(r) { return r.json(); }).then(function(d) {
    setConn(true);
    setVal('s-printer', d.printer ? 'Ready' : 'Not ready', d.printer ? 'on' : 'off');
    setVal('s-pump', d.pump ? 'Running' : 'Stopped', d.pump ? 'on' : 'off');
    setVal('s-dispensing', d.dispensing ? 'Yes' : 'No', d.dispensing ? 'on' : 'off');
    setVal('s-ir', d.irSensor ? 'Detected' : 'Clear', d.irSensor ? 'on' : 'off');
    setVal('s-moisture', d.moisture + '%');
    setVal('s-light', d.light + '%');
    setVal('s-sanitizer', d.sanitizer + '%');
    setVal('s-led', d.ledBrightness + '/255');
    setVal('s-dispenses', d.dispenses);
  }).catch(function() {
    setConn(false);
  });
}

document.getElementById('apiKey').value = getKey();
refreshStatus();
setInterval(refreshStatus, 2000);
</script>
</body>
</html>
)HTMLPAGE";
    server_.send_P(200, "text/html", PAGE);
}

void TestServer::begin() {
    // WebServer only retains headers explicitly registered here; without this,
    // server_.header("X-API-Key") always returns empty regardless of what the client sends.
    static const char* headerKeys[] = {"X-API-Key"};
    server_.collectHeaders(headerKeys, 1);

    server_.on("/", HTTP_GET, [this]() { handleRoot(); });
    server_.on("/status", HTTP_GET, [this]() { handleStatus(); });
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
    server_.on("/settings/auto-brightness", HTTP_GET, [this]() { handleSetAutoBrightness(); });
    server_.on("/settings/light-calibration", HTTP_GET, [this]() { handleSetLightCalibration(); });
    server_.begin();
    Logger::info("Main", "Hardware test server on port 8081 (auth: X-API-Key header or ?key=, matches WEB_PASSWORD)");
}
