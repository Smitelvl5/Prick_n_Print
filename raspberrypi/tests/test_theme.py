"""
The four UIs share one theme: the TZT screen (include/UiTheme.h), the two ESP32
web pages (include/WebTheme.h) and this console (static/theme.css). These tests
fail if the copies drift. Skipped when only raspberrypi/ is checked out (on the Pi).
"""

import re
from pathlib import Path

import pytest

PI_DIR = Path(__file__).resolve().parent.parent
REPO = PI_DIR.parent
CSS = PI_DIR / "static" / "theme.css"
WEB_THEME_H = REPO / "include" / "WebTheme.h"
UI_THEME_H = REPO / "include" / "UiTheme.h"

needs_firmware = pytest.mark.skipif(
    not (WEB_THEME_H.exists() and UI_THEME_H.exists()), reason="firmware headers not checked out"
)


@needs_firmware
def test_web_theme_header_matches_css():
    header = WEB_THEME_H.read_text()
    m = re.search(r'R"CSS\((.*)\)CSS"', header, re.S)
    assert m, "WEB_THEME_CSS raw string not found"
    assert m.group(1) == CSS.read_text(), "include/WebTheme.h and static/theme.css differ - copy one over the other"


@needs_firmware
def test_css_palette_matches_screen():
    screen = {
        name.lower().replace("_", "-"): value.upper()
        for name, value in re.findall(r"UI_([A-Z_]+)\s*=\s*0x([0-9A-Fa-f]{6})", UI_THEME_H.read_text())
    }
    web = {name: value.upper() for name, value in re.findall(r"--fi-([a-z-]+):\s*#([0-9A-Fa-f]{6})", CSS.read_text())}
    assert screen, "no colours parsed from UiTheme.h"
    assert web == screen
