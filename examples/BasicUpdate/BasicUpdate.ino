#include <Arduino.h>
#include <WiFi.h>
#include <SPIFFS.h>

#include <ESPFileUpdater.h>

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

void setup() {
    Serial.begin(115200);
    delay(1000);

    // Initialize SPIFFS
    if (!SPIFFS.begin(true)) {
        Serial.println("SPIFFS Mount Failed");
        return;
    }

    // Connect to WiFi
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("\nWiFi connected!");

    // Set time via NTP for maxAge support
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    time_t now = time(nullptr);
    while (now < 100000) {
        delay(100);
        now = time(nullptr);
    }
    Serial.println("Time synchronized!");

    // Create updater instance
    ESPFileUpdater updater(SPIFFS);

    // Optional: configure settings
    updater.setMaxSize(1024);
    updater.setUserAgent("MyESPProject/1.0");
    updater.setRetryCount(2);
    updater.setRetryDelay(2000);

    // Run update — checks weekly, logs verbosely
    ESPFileUpdater::UpdateStatus result = updater.checkAndUpdate(
      "/www/timezones.json.gz",
      "https://raw.githubusercontent.com/trip5/timezones.json/master/timezones.json.gz",
      "7d",
      true
    );

    // Handle update result
    if (result == ESPFileUpdater::UPDATED) {
        Serial.println("File was updated successfully.");
    } else if (result == ESPFileUpdater::NOT_MODIFIED || result == ESPFileUpdater::MAX_AGE_NOT_REACHED) {
        Serial.println("No update needed.");
    } else if (result == ESPFileUpdater::OUT_OF_MEMORY) {
        Serial.println("Insufficient heap memory for operation.");
    } else {
        Serial.println("Update failed.");
    }
}

void loop() {
    // Your main code here
}
