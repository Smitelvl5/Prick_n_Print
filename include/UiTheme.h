#ifndef UI_THEME_H
#define UI_THEME_H

// ============================================================================
// TZT display design system - ported from the Spectral sensor's TFT_Display
// (same component family: card-tone header with a "<" back
// target, rounded card rows/tiles, app-style switches, primary/secondary
// buttons), scaled from its 480x320 panel to this 320x240 one.
// ============================================================================

#include <lvgl.h>

// Cactus palette: cactus green (UI_BORDER) is the main colour; backgrounds, text and
// icons are tinted from it. Roles are unchanged from the FI palette this replaced.
// Same values as --fi-* in raspberrypi/static/theme.css (test_theme.py checks it).
enum UiColor : uint32_t {
    UI_BACKGROUND     = 0x131B15,  // page background
    UI_CARD           = 0x1C2A1F,  // header band, cards, list rows, tiles
    UI_FIELD          = 0x26352A,  // value wells, bar/slider tracks, switch off
    UI_BORDER         = 0x5B7F4A,  // cactus green: primary accent - filled primary buttons, switch on
    UI_ACCENT         = 0xB4D69B,  // icons, "connected" status
    UI_TEXT           = 0xC8D4C2,  // universal foreground
    UI_TEXT_MUTED     = 0x7E9478,  // captions, secondary labels, chevrons
    UI_TITLE          = 0xFFFFFF,  // text on filled primary buttons
    UI_WARNING        = 0xE74C3C,  // errors / destructive / disconnected
    UI_DANGER_DEEP    = 0x7B241C,  // destructive button while idle
    UI_SUCCESS        = 0x2FB36B,  // brighter emerald so "OK" doesn't read as the brand green
    UI_AMBER          = 0xD4AC0D,  // mid tier, "needs attention" dot
    UI_SELECTION      = 0x4A6B3F,  // tap-feedback flash
    UI_BUTTON         = 0x2E4A30   // secondary button fill
};

static inline lv_color_t uiColor(UiColor c) { return lv_color_hex(c); }

// Shared metrics so every card, row, tile and button reads as one family.
// Spectral uses MARGIN 12 / RADIUS 10 / 52px rows at 480x320; 2/3 of that
// here, but rows and back targets stay >= 44px for the resistive panel.
#define UI_MARGIN      8
#define UI_GAP         6
#define UI_RADIUS      8
#define UI_ROW_H       44   // list row card height (touch target)
#define UI_BACK_W      44   // header "<" tap target width
#define UI_SWITCH_W    40
#define UI_SWITCH_H    22
#define UI_DOT_R       5    // "needs attention" dot on a tile

#endif // UI_THEME_H
