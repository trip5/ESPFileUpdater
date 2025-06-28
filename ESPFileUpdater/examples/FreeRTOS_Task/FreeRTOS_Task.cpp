#include "../ESPFileUpdater/ESPFileUpdater.h"

// This example from network.cpp in my update to yoRadio (simple, early example)

bool wasUpdated(ESPFileUpdater::UpdateStatus status) { return status == ESPFileUpdater::UPDATED; }
ESPFileUpdater updater(SPIFFS);

// 

void updateTZjson(void* param) {
  Serial.println("[ESPFileUpdater: Timezones.json] Started update.");
  ESPFileUpdater* updater = (ESPFileUpdater*)param;
  ESPFileUpdater::UpdateStatus result = updater->checkAndUpdate(
      "/www/timezones.json.gz",
      TIMEZONES_JSON_GZ_URL,
      "1 week", // update once a week at most
      false // verbose logging
  );
  if (result == ESPFileUpdater::UPDATED) {
    Serial.println("[ESPFileUpdater: Timezones.json] Update completed.");
  } else if (result == ESPFileUpdater::NOT_MODIFIED||result == ESPFileUpdater::MAX_AGE_NOT_REACHED) {
    Serial.println("[ESPFileUpdater: Timezones.json] No update needed.");
  } else {
    Serial.println("[ESPFileUpdater: Timezones.json] Update failed.");
  }
}

void startAsyncServices(void* param){
   //other task
  updateTZjson(param);
  vTaskDelete(NULL);
}

void Config::startAsyncServicesButWait() {
  // called from setup() after SPIFFS and Wi-Fi have been initialized and checked...
  if (WiFi.status() != WL_CONNECTED) return;
  ESPFileUpdater* updater = nullptr;
  updater = new ESPFileUpdater(SPIFFS);
  xTaskCreate(startAsyncServices, "startAsyncServices", 8192, updater, 1, NULL); // pass pointer
}
