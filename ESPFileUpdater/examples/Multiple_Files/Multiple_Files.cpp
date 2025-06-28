#include "../ESPFileUpdater/ESPFileUpdater.h"

// This example from network.cpp in my update to yoRadio

bool wasUpdated(ESPFileUpdater::UpdateStatus status) { return status == ESPFileUpdater::UPDATED; }
ESPFileUpdater updater(SPIFFS);

// 

void updateFile(void* param, const char* localFile, const char* onlineFile, const char* updatePeriod, const char* simpleName) {
  char startMsg[128];
  snprintf(startMsg, sizeof(startMsg), "[ESPFileUpdater: %s] Started update.", simpleName);
  Serial.println(startMsg);
  ESPFileUpdater* updater = (ESPFileUpdater*)param;
  ESPFileUpdater::UpdateStatus result = updater->checkAndUpdate(
      localFile,
      onlineFile,
      updatePeriod,
      false // verbose logging
  );
  if (result == ESPFileUpdater::UPDATED) {
    Serial.printf("[ESPFileUpdater: %s] Update completed.\n", simpleName);
  } else if (result == ESPFileUpdater::NOT_MODIFIED||result == ESPFileUpdater::MAX_AGE_NOT_REACHED) {
    Serial.printf("[ESPFileUpdater: %s] No update needed.\n", simpleName);
  } else {
    Serial.printf("[ESPFileUpdater: %s] Update failed.\n", simpleName);
  }
}

void startAsyncServices(void* param){
  updateFile(param, "/www/timezones.json.gz", TIMEZONES_JSON_URL, "1 week", "Timezones file");
  updateFile(param, "/www/rb_srvrs.json", RADIO_BROWSER_SERVERS_URL, "4 weeks", "Radio Browser Servers List");  
  vTaskDelete(NULL);
}