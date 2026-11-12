#ifndef WIFI_PORTAL_H
#define WIFI_PORTAL_H

// Wi-Fi setup shared by both boards: the WiFiManager portal in the cactus theme, and a connect that
// survives a power cut. The boards boot faster than a router, so a failed first try with Wi-Fi
// already saved must NOT open the setup portal and then sit offline: it keeps retrying in the
// background instead. The portal only opens when nothing is saved yet, or when asked for.

#include <WiFi.h>
#include <WiFiManager.h>

// WiFiManager's page, restyled with the palette of include/UiTheme.h / include/WebTheme.h
// (its own <style> comes first, so these rules win). Only Wi-Fi setup is offered: no info,
// erase or firmware-update pages.
static const char WIFI_PORTAL_HEAD[] PROGMEM = R"HTML(<meta name="viewport" content="width=device-width,initial-scale=1"><style>
body{background:#131B15;color:#C8D4C2;font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif;margin:0;padding:16px}
.wrap{width:100%;max-width:420px;min-width:0}
h1{color:#fff;font-size:1.5rem;margin:8px 0 2px}h3{color:#A9BCA2;font-weight:500;font-size:.95rem;margin:0 0 16px}
h1:after{content:"";display:block;width:48px;height:3px;border-radius:2px;background:#B4D69B;margin:10px 0 0}
a{color:#B4D69B}a:hover{color:#fff}
div,input,select{margin:6px 0}
input,select{background:#26352A;color:#fff;border:1px solid #3A5A2E;border-radius:10px;padding:12px;font-size:1rem}
input:focus{outline:2px solid #B4D69B;border-color:transparent}
label{color:#A9BCA2;font-size:.9rem}
button,input[type=submit]{background:#3A5A2E;color:#fff;border-radius:10px;font-weight:600;line-height:2.8rem;font-size:1.05rem}
button.D{background:#7B241C}
#wifi_list>div,form>div>a,.ql>div{background:#1C2A1F;border-radius:10px;padding:12px;margin:6px 0}
.msg{background:#1C2A1F;color:#C8D4C2;border:0;border-left:4px solid #B4D69B;border-radius:10px}
.msg.D{border-left-color:#E74C3C}.msg.S{border-left-color:#2FB36B}.msg h4{color:#fff}
.q{filter:invert(1) opacity(.8)}
hr{border:0;border-top:1px solid #26352A}
</style>)HTML";

static void styleWifiPortal(WiFiManager& wm, const char* title) {
    wm.setTitle(title);
    wm.setCustomHeadElement(WIFI_PORTAL_HEAD);
    static std::vector<const char*> menu = { "wifi", "exit" };
    wm.setMenu(menu);
    wm.setShowInfoUpdate(false);
    wm.setShowInfoErase(false);
    wm.setScanDispPerc(true);  // "72%" instead of an icon only
}

// Make sure the board keeps trying saved Wi-Fi by itself (it may not be up yet, or come back later)
static void keepRetryingWifi() {
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(true);
    WiFi.begin();  // the saved network
}

// Boot-time connect. Saved Wi-Fi: up to ~60 s of tries, then carry on and keep retrying in the
// background. Nothing saved (first setup): open the portal for portalTimeoutS seconds.
// Returns true when connected now.
static bool connectWifi(WiFiManager& wm, const char* apSsid, const char* apPassword, uint16_t portalTimeoutS) {
    bool saved = wm.getWiFiIsSaved();
    if (saved) {
        wm.setEnableConfigPortal(false);
        wm.setConnectTimeout(20);
        wm.setConnectRetries(3);
    } else {
        wm.setConfigPortalTimeout(portalTimeoutS);
    }
    bool ok = wm.autoConnect(apSsid, apPassword);
    if (!ok && (saved || wm.getWiFiIsSaved())) keepRetryingWifi();
    return ok;
}

// Setup portal on request (e.g. Settings > Wi-Fi > Change). If nobody finishes it in time, the
// old network is still saved and gets retried as usual.
static bool openWifiPortal(WiFiManager& wm, const char* apSsid, const char* apPassword, uint16_t portalTimeoutS) {
    wm.setConfigPortalTimeout(portalTimeoutS);
    bool ok = wm.startConfigPortal(apSsid, apPassword);
    if (!ok && wm.getWiFiIsSaved()) keepRetryingWifi();
    return ok;
}

#endif // WIFI_PORTAL_H
