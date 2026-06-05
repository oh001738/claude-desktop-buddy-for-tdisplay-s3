#pragma once
#include <Arduino.h>

// Launches WiFiManager Config Portal to configure WiFi on the device.
void startWifiPortal();

// Initiates the Wi-Fi connection and OTA update flow.
// Calls progressCallback with status messages and percentage progress (0-100).
// Returns true on success (though it will typically hard reset the ESP32),
// and false on failure, copying the error message into errBuf.
bool otaUpdateFlow(void (*progressCallback)(const char* status, int progress), char* errBuf, size_t errBufLen);
