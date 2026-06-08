#include "ota.h"
#include "stats.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <WiFiManager.h>

const char* CURRENT_VERSION = "v1.1.2";
const char* GITHUB_API_URL = "https://api.github.com/repos/oh001738/claude-desktop-buddy-for-tdisplay-s3/releases/latest";

static void (*_progCb)(const char* status, int progress) = nullptr;
extern void drawWifiPortalScreen(const char* apName);

// Cleanly shut down Wi-Fi and persist the OFF state to NVS
static void wifiShutdown() {
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    settings().wifi = false;
    settingsSave();
}

static int last_pct = -1;
static void update_progress(int cur, int total) {
    if (_progCb && total > 0) {
        int pct = (int)(((int64_t)cur * 100) / total);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        if (pct != last_pct) {
            last_pct = pct;
            _progCb("Downloading", pct);
        }
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
    last_pct = -1;

    progressCallback("Connecting Wi-Fi", 0);
    
    // Initialize WiFi and begin connection to last saved network
    WiFi.mode(WIFI_STA);
    WiFi.begin();
    
    // Poll until NVS credentials are loaded (up to 1 second, 50ms steps)
    // WiFi.begin() triggers an async NVS read; a fixed delay can be too short
    String storedSsid = "";
    uint32_t ssidWaitStart = millis();
    while (millis() - ssidWaitStart < 1000) {
        storedSsid = WiFi.SSID();
        if (storedSsid.length() > 0) break;
        delay(50);
    }

    if (storedSsid.length() == 0) {
        snprintf(errBuf, errBufLen, "Wi-Fi not configured. Go to menu and turn Wi-Fi ON.");
        // Don't persist wifi=false here — the user hasn't configured it yet
        WiFi.disconnect(true);
        WiFi.mode(WIFI_OFF);
        return false;
    }

    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        int elapsed = (millis() - startMs) / 1000;
        if (elapsed > 15) {
            snprintf(errBuf, errBufLen, "Wi-Fi Timeout (SSID: %s)", storedSsid.c_str());
            wifiShutdown();
            return false;
        }
        int prog = (elapsed * 90) / 15;
        progressCallback("Connecting Wi-Fi", prog);
    }

    progressCallback("Checking updates", 0);

    WiFiClientSecure apiClient;
    apiClient.setInsecure(); // Bypass GitHub root cert validation for permanent compatibility
    
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    
    // GitHub API requires User-Agent
    if (!http.begin(apiClient, GITHUB_API_URL)) {
        snprintf(errBuf, errBufLen, "Failed to connect to GitHub API");
        wifiShutdown();
        return false;
    }
    http.addHeader("User-Agent", "ESP32-OTA-Client");

    int httpCode = http.GET();
    if (httpCode != HTTP_CODE_OK) {
        snprintf(errBuf, errBufLen, "GitHub API HTTP Error: %d", httpCode);
        http.end();
        wifiShutdown();
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
        wifiShutdown();
        return false;
    }

    const char* latestVersion = doc["tag_name"];
    if (!latestVersion) {
        snprintf(errBuf, errBufLen, "No tag_name found in release");
        wifiShutdown();
        return false;
    }

    if (strcmp(latestVersion, CURRENT_VERSION) == 0) {
        snprintf(errBuf, errBufLen, "Up to date (%s)", CURRENT_VERSION);
        wifiShutdown(); // Turn off Wi-Fi and persist OFF state even when already up to date
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
        wifiShutdown();
        return false;
    }

    progressCallback("Debugging OTA", 0);
    // DEBUG: Test downloadUrl and fill errBuf with details if it's not a valid binary
    {
        WiFiClientSecure testClient;
        testClient.setInsecure();
        HTTPClient testHttp;
        testHttp.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
        if (!testHttp.begin(testClient, downloadUrl)) {
            snprintf(errBuf, errBufLen, "DBG: testHttp begin fail");
            wifiShutdown();
            return false;
        }
        testHttp.addHeader("User-Agent", "ESP32-OTA-Client");
        int code = testHttp.GET();
        if (code != 200) {
            snprintf(errBuf, errBufLen, "DBG HTTP: %d", code);
            testHttp.end();
            wifiShutdown();
            return false;
        }
        
        WiFiClient* stream = testHttp.getStreamPtr();
        uint8_t buf[16] = {0};
        uint32_t t = millis();
        while (stream->available() < 16 && millis() - t < 3000) {
            delay(10);
        }
        int r = stream->readBytes(buf, 16);
        if (r < 16) {
            snprintf(errBuf, errBufLen, "DBG Read: %d B", r);
            testHttp.end();
            wifiShutdown();
            return false;
        }
        
        if (buf[0] != 0xE9) {
            buf[15] = 0; // null terminate
            // Clean up non-printable chars
            for (int i = 0; i < 15; i++) {
                if (buf[i] < 32 || buf[i] > 126) buf[i] = '.';
            }
            snprintf(errBuf, errBufLen, "Not bin: %02X %02X / %s", 
                     buf[0], buf[1], (char*)buf);
            testHttp.end();
            wifiShutdown();
            return false;
        }
        testHttp.end();
    }

    progressCallback("Downloading", 0);
    httpUpdate.onProgress(update_progress);
    httpUpdate.rebootOnUpdate(false); // Manual reboot so we can save settings first
    httpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

    WiFiClientSecure updateClient;
    updateClient.setInsecure();
    t_httpUpdate_return ret = httpUpdate.update(updateClient, downloadUrl);

    if (ret == HTTP_UPDATE_OK) {
        // OTA succeeded — turn off Wi-Fi, persist the setting, then reboot
        wifiShutdown();
        delay(300);
        ESP.restart();
    }

    // Reached only on failure
    if (ret == HTTP_UPDATE_FAILED) {
        snprintf(errBuf, errBufLen, "OTA fail: (%d) %s", 
                 httpUpdate.getLastError(), httpUpdate.getLastErrorString().c_str());
    } else if (ret == HTTP_UPDATE_NO_UPDATES) {
        snprintf(errBuf, errBufLen, "No updates available");
    }

    wifiShutdown();
    return false;
}
