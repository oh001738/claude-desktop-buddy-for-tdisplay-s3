#include "ota.h"
#include "stats.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

const char* CURRENT_VERSION = "v1.0.3";
const char* GITHUB_API_URL = "https://api.github.com/repos/oh001738/claude-desktop-buddy-for-tdisplay-s3/releases/latest";

static void (*_progCb)(const char* status, int progress) = nullptr;
extern void drawWifiPortalScreen(const char* apName);

static void update_progress(int cur, int total) {
    if (_progCb && total > 0) {
        int pct = (int)(((int64_t)cur * 100) / total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        _progCb("Downloading", pct);
    }
}

void startWifiPortal() {
    WiFiManager wm;
    
    // Set custom callback to draw our own screen when AP is active
    wm.setAPCallback([](WiFiManager* myWiFiManager) {
        drawWifiPortalScreen(myWiFiManager->getConfigPortalSSID().c_str());
    });

    char apName[32];
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(apName, sizeof(apName), "Claude-%02X%02X", mac[4], mac[5]);
    
    // Set config portal timeout to 120 seconds
    wm.setConfigPortalTimeout(120);
    
    // Auto connect or start config portal
    bool res = wm.autoConnect(apName);
    
    if (!res) {
        // Portal timeout, turn off Wi-Fi setting
        settings().wifi = false;
        settingsSave();
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
    } else {
        // Successfully configured and connected
        settings().wifi = true;
        settingsSave();
    }
}

bool otaUpdateFlow(void (*progressCallback)(const char* status, int progress), char* errBuf, size_t errBufLen) {
    _progCb = progressCallback;
    errBuf[0] = 0;

    // Check if WiFi settings are stored in SDK
    String storedSsid = WiFi.SSID();
    if (storedSsid.length() == 0) {
        snprintf(errBuf, errBufLen, "Wi-Fi not configured. Go to menu and turn Wi-Fi ON.");
        return false;
    }

    progressCallback("Connecting Wi-Fi", 0);
    
    // Begin connection using saved credentials in SDK
    WiFi.disconnect(true);
    delay(200);
    WiFi.mode(WIFI_STA);
    WiFi.begin();

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        int elapsed = (millis() - startMs) / 1000;
        if (elapsed > 15) {
            snprintf(errBuf, errBufLen, "Wi-Fi Timeout (SSID: %s)", storedSsid.c_str());
            WiFi.disconnect(true);
            return false;
        }
        int prog = (elapsed * 90) / 15;
        progressCallback("Connecting Wi-Fi", prog);
    }

    progressCallback("Checking updates", 0);

    WiFiClientSecure client;
    client.setInsecure(); // Bypass GitHub root cert validation for permanent compatibility
    
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    // GitHub API requires User-Agent
    if (!http.begin(client, GITHUB_API_URL)) {
        snprintf(errBuf, errBufLen, "Failed to connect to GitHub API");
        WiFi.disconnect(true);
        return false;
    }
    http.addHeader("User-Agent", "ESP32-OTA-Client");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        snprintf(errBuf, errBufLen, "GitHub API HTTP Error: %d", httpCode);
        http.end();
        WiFi.disconnect(true);
        return false;
    }

    // Parse latest release info using json filter to conserve RAM
    JsonDocument filter;
    filter["tag_name"] = true;
    JsonArray assetsFilter = filter["assets"].to<JsonArray>();
    JsonObject assetFilter = assetsFilter.add<JsonObject>();
    assetFilter["name"] = true;
    assetFilter["browser_download_url"] = true;

    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
    http.end();

    if (error) {
        snprintf(errBuf, errBufLen, "JSON parse error: %s", error.c_str());
        WiFi.disconnect(true);
        return false;
    }

    const char* latestVersion = doc["tag_name"];
    if (!latestVersion) {
        snprintf(errBuf, errBufLen, "No tag_name found in release");
        WiFi.disconnect(true);
        return false;
    }

    if (strcmp(latestVersion, CURRENT_VERSION) == 0) {
        snprintf(errBuf, errBufLen, "Up to date (%s)", CURRENT_VERSION);
        WiFi.disconnect(true);
        return false;
    }

    const char* downloadUrl = nullptr;
    JsonArray assets = doc["assets"];
    for (JsonObject asset : assets) {
        const char* name = asset["name"];
        if (name && strcmp(name, "firmware.bin") == 0) {
            downloadUrl = asset["browser_download_url"];
            break;
        }
    }

    if (!downloadUrl) {
        snprintf(errBuf, errBufLen, "firmware.bin not found in release %s", latestVersion);
        WiFi.disconnect(true);
        return false;
    }

    progressCallback("Downloading", 0);
    httpUpdate.onProgress(update_progress);
    httpUpdate.rebootOnUpdate(true);
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    t_httpUpdate_return ret = httpUpdate.update(client, downloadUrl);

    if (ret == HTTP_UPDATE_FAILED) {
        snprintf(errBuf, errBufLen, "OTA fail: (%d) %s", 
                 httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    } else if (ret == HTTP_UPDATE_NO_UPDATES) {
        snprintf(errBuf, errBufLen, "No updates available");
    }

    WiFi.disconnect(true);
    return false;
}
