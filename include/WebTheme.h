#ifndef WEB_THEME_H
#define WEB_THEME_H

// Shared web theme for the ESP32 pages (TZT web UI + login, Main ESP32 bench page).
// Identical copy of raspberrypi/static/theme.css (the Pi console's) - edit both;
// raspberrypi/tests/test_theme.py fails if they drift. Palette = include/UiTheme.h.

#include <Arduino.h>
#include <WebServer.h>
#include <initializer_list>

static const char WEB_THEME_CSS[] PROGMEM = R"CSS(/* Prick'n'Print shared web theme - TZT web UI, Main ESP32 bench page, Pi console.
   Same cactus palette and component family as the TZT screen (include/UiTheme.h).
   Two copies: this file (Pi) and include/WebTheme.h (both ESP32s) - keep them identical;
   raspberrypi/tests/test_theme.py checks it. */
:root {
  /* Cactus palette (UiTheme.h); --fi-border is the cactus green */
  --fi-background: #131B15; --fi-card: #1C2A1F; --fi-field: #26352A; --fi-border: #5B7F4A;
  --fi-accent: #B4D69B; --fi-text: #C8D4C2; --fi-text-muted: #7E9478; --fi-title: #FFFFFF;
  --fi-warning: #E74C3C; --fi-danger-deep: #7B241C; --fi-success: #2FB36B; --fi-amber: #D4AC0D;
  --fi-selection: #4A6B3F; --fi-button: #2E4A30;

  /* Roles used by the pages */
  --bg: var(--fi-background);
  --surface: var(--fi-card);
  --surface-2: var(--fi-field);
  --surface-hover: color-mix(in srgb, var(--fi-selection) 35%, var(--fi-card));
  --border: color-mix(in srgb, var(--fi-border) 22%, var(--fi-background));
  --border-strong: color-mix(in srgb, var(--fi-border) 45%, var(--fi-background));
  --text: var(--fi-text);
  --text-muted: color-mix(in srgb, var(--fi-text) 60%, var(--fi-text-muted));
  --text-faint: var(--fi-text-muted);
  --accent: var(--fi-border);
  --accent-hover: var(--fi-selection);
  --accent-light: var(--fi-accent);
  --accent-dim: color-mix(in srgb, var(--fi-border) 18%, transparent);
  --button: var(--fi-button);
  --ok: var(--fi-success);
  --ok-dim: color-mix(in srgb, var(--fi-success) 25%, transparent);
  --warn: var(--fi-amber);
  --danger: var(--fi-warning);
  --danger-hover: color-mix(in srgb, var(--fi-warning) 80%, var(--fi-danger-deep));
  --danger-deep: var(--fi-danger-deep);
  --danger-dim: color-mix(in srgb, var(--fi-warning) 16%, transparent);
  --shadow: 0 8px 24px rgba(4, 10, 5, 0.5);
  --radius: 10px;
  --radius-sm: 8px;
  --font-ui: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  --font-mono: "JetBrains Mono", "SF Mono", ui-monospace, Consolas, monospace;
  color-scheme: dark;
}
* { box-sizing: border-box; margin: 0; padding: 0; -webkit-tap-highlight-color: transparent; }
html { -webkit-text-size-adjust: 100%; }
body {
  background: var(--bg); color: var(--text); font-family: var(--font-ui);
  line-height: 1.45; min-height: 100vh; min-height: 100dvh; -webkit-font-smoothing: antialiased;
}
a { color: var(--accent-light); }

/* Header band: card tone, title left, status right (the screen's status bar) */
.topbar {
  position: sticky; top: 0; z-index: 50;
  display: flex; align-items: center; gap: 12px; min-height: 56px;
  padding: max(8px, env(safe-area-inset-top)) max(16px, env(safe-area-inset-right)) 8px max(16px, env(safe-area-inset-left));
  background: var(--surface); border-bottom: 1px solid var(--border);
}
.topbar h1 { font-size: 1.05rem; font-weight: 700; color: var(--text); letter-spacing: -0.01em; }
.topbar .sub { font-size: 0.75rem; color: var(--text-faint); }
.topbar .spacer { flex: 1; }
.page { max-width: 1040px; margin: 0 auto; padding: 16px max(16px, env(safe-area-inset-right)) 40px max(16px, env(safe-area-inset-left)); }

/* Cards and section captions */
.card { background: var(--surface); border-radius: var(--radius); padding: 16px; }
.caption, .card h2 {
  font-size: 0.72rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.06em;
  color: var(--text-muted); margin-bottom: 12px;
}

/* Buttons: primary = the next step, secondary = button fill, danger = destructive */
.btn {
  display: inline-flex; align-items: center; justify-content: center; gap: 6px;
  min-height: 40px; padding: 8px 16px; border: 1px solid transparent; border-radius: var(--radius-sm);
  background: var(--button); color: var(--text); font: inherit; font-size: 0.875rem; font-weight: 600;
  cursor: pointer; text-decoration: none; white-space: nowrap; user-select: none;
  transition: background 0.15s, border-color 0.15s, color 0.15s, transform 0.05s;
}
.btn:hover { background: var(--fi-selection); }
.btn:active { transform: scale(0.97); }
.btn:focus-visible { outline: 2px solid var(--accent-light); outline-offset: 2px; }
.btn:disabled { opacity: 0.5; cursor: default; transform: none; }
.btn-primary { background: var(--accent); color: var(--fi-title); }
.btn-primary:hover { background: var(--accent-hover); }
.btn-ghost { background: transparent; color: var(--text-muted); border-color: var(--border); }
.btn-ghost:hover { background: var(--surface-hover); color: var(--text); border-color: var(--border-strong); }
.btn-danger { background: transparent; color: var(--danger); border-color: color-mix(in srgb, var(--danger) 45%, var(--border)); }
.btn-danger:hover { background: var(--danger-dim); border-color: var(--danger); }
.btn-danger.solid { background: var(--danger); color: var(--fi-title); border-color: var(--danger); }
.btn-danger.solid:hover { background: var(--danger-hover); }
.btn.confirming { background: var(--danger-deep) !important; color: var(--fi-title) !important; border-color: var(--danger) !important; }
.btn-row { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
.btn-row > .btn.grow { flex: 1; }

/* Status pill: dot + label; connected reads in accent like the screen's WiFi glyph */
.status-pill {
  display: inline-flex; align-items: center; gap: 6px; padding: 4px 11px; border-radius: 999px;
  background: var(--surface-2); border: 1px solid var(--border);
  font-size: 0.75rem; font-family: var(--font-mono); color: var(--text-muted); white-space: nowrap;
}
.status-pill .dot { width: 7px; height: 7px; border-radius: 50%; background: var(--text-faint); flex-shrink: 0; }
.status-pill.ok { color: var(--accent-light); border-color: var(--border-strong); }
.status-pill.ok .dot { background: var(--accent-light); box-shadow: 0 0 0 3px var(--accent-dim); }
.status-pill.warn { color: var(--warn); }
.status-pill.warn .dot { background: var(--warn); }
.status-pill.bad { color: var(--danger); border-color: color-mix(in srgb, var(--danger) 40%, var(--border)); }
.status-pill.bad .dot { background: var(--danger); box-shadow: 0 0 0 3px var(--danger-dim); }

/* Tile grid (the screen's launcher / Spectral's settings grid) */
.tile-grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(140px, 1fr)); gap: 10px; }
.tile { position: relative; background: var(--surface); border-radius: var(--radius); padding: 12px 14px; min-height: 76px; }
.card .tile { background: var(--surface-2); }
.tile .label { font-size: 0.7rem; font-weight: 600; text-transform: uppercase; letter-spacing: 0.05em; color: var(--text-faint); }
.tile .value { font-size: 1.3rem; font-weight: 700; margin-top: 4px; color: var(--text); font-variant-numeric: tabular-nums; }
.tile .value.small { font-size: 0.95rem; }
.tile .value.on { color: var(--accent-light); }
.tile .value.off { color: var(--text-faint); }
.tile .value.warn { color: var(--warn); }
.tile .value.bad { color: var(--danger); }
.tile .bar { height: 6px; border-radius: 3px; background: var(--surface-2); margin-top: 8px; overflow: hidden; }
.tile .bar > i { display: block; height: 100%; width: 0; background: var(--ok); border-radius: 3px; transition: width 0.3s; }
.tile .bar > i.warn { background: var(--warn); }
.tile .bar > i.bad { background: var(--danger); }
/* Something that needs doing shows as a dot on its tile, not a sentence */
.tile.attention::after {
  content: ""; position: absolute; top: 12px; right: 12px; width: 9px; height: 9px;
  border-radius: 50%; background: var(--warn);
}

/* List rows: card rows on the page background, actions on the right */
.list { display: flex; flex-direction: column; gap: 6px; }
.list-row {
  display: flex; align-items: center; justify-content: space-between; gap: 12px;
  min-height: 48px; padding: 10px 12px 10px 14px; background: var(--surface-2); border-radius: var(--radius-sm);
  font-size: 0.9rem;
}
.card .list-row { background: var(--surface-2); }
.list-row .text { min-width: 0; overflow-wrap: anywhere; }
.list-row .meta { color: var(--text-faint); font-size: 0.75rem; font-family: var(--font-mono); margin-top: 2px; }
.list-row .actions { display: flex; gap: 6px; flex-shrink: 0; }
.list-row .btn { min-height: 34px; padding: 6px 12px; font-size: 0.8rem; }
.empty {
  color: var(--text-faint); font-size: 0.85rem; text-align: center; padding: 22px 12px;
  border: 1px dashed var(--border); border-radius: var(--radius-sm);
}

/* Fields (value wells) */
input[type=text], input[type=password], input[type=number], input[type=time], input[type=datetime-local], select, textarea {
  min-height: 40px; padding: 8px 12px; background: var(--surface-2); color: var(--text);
  border: 1px solid var(--border); border-radius: var(--radius-sm); font: inherit; font-size: 16px;
}
input:focus, select:focus, textarea:focus { outline: none; border-color: var(--accent); box-shadow: 0 0 0 3px var(--accent-dim); }
input::placeholder, textarea::placeholder { color: var(--text-faint); }
input[type=range] { accent-color: var(--accent); }
label { font-size: 0.8rem; color: var(--text-muted); }

/* App-style switch: <label class="switch"><input type="checkbox"><span></span></label> */
.switch { position: relative; display: inline-block; width: 46px; height: 26px; flex-shrink: 0; cursor: pointer; }
.switch input { position: absolute; opacity: 0; width: 0; height: 0; }
.switch span {
  position: absolute; inset: 0; border-radius: 13px; background: var(--surface-2);
  border: 1px solid var(--text-faint); transition: background 0.15s, border-color 0.15s;
}
.switch span::before {
  content: ""; position: absolute; left: 5px; top: 50%; width: 14px; height: 14px; margin-top: -7px;
  border-radius: 50%; background: var(--text-faint); transition: transform 0.15s, background 0.15s, width 0.15s;
}
.switch input:checked + span { background: var(--accent); border-color: var(--accent); }
.switch input:checked + span::before { transform: translateX(19px); background: var(--fi-title); width: 18px; height: 18px; margin-top: -9px; left: 3px; }
.switch input:focus-visible + span { outline: 2px solid var(--accent-light); outline-offset: 2px; }

/* Toast */
.toast {
  position: fixed; bottom: max(20px, env(safe-area-inset-bottom)); right: 20px; z-index: 1000;
  padding: 10px 16px; background: var(--surface); border: 1px solid var(--accent); border-radius: var(--radius-sm);
  color: var(--accent-light); font-size: 0.85rem; font-weight: 600; box-shadow: var(--shadow);
}
.toast.err { border-color: var(--danger); color: var(--danger); }

::-webkit-scrollbar { width: 10px; height: 10px; }
::-webkit-scrollbar-track { background: transparent; }
::-webkit-scrollbar-thumb { background: var(--border-strong); border-radius: 6px; }
@media (prefers-reduced-motion: reduce) { * { transition: none !important; animation: none !important; } }
)CSS";

// Streams a page from flash in parts (e.g. head-up-to-<style>, WEB_THEME_CSS, rest)
// so the shared CSS isn't copied into each page or into a heap String.
static inline void sendHtmlParts(WebServer& server, std::initializer_list<const char*> parts) {
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", "");
    for (const char* p : parts) server.sendContent_P(p);
    server.sendContent("");
}

#endif // WEB_THEME_H
