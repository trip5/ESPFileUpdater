#include <ESPFileUpdater.h>

// This example from network.cpp in my update to yoRadio

static const char* requiredFiles[] = {"dragpl.js.gz","ir.css.gz","irrecord.html.gz","ir.js.gz","logo.svg.gz","options.html.gz","script.js.gz",
                                     "timezones.json.gz","rb_srvrs.json","search.html.gz","search.js.gz","search.css.gz",
                                     "style.css.gz","updform.html.gz","theme.css","player.html.gz"}; // keep main page at end
static const size_t requiredFilesCount = sizeof(requiredFiles) / sizeof(requiredFiles[0]);

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
      ESPFILEUPDATER_VERBOSE
  );
  if (result == ESPFileUpdater::UPDATED) {
    Serial.printf("[ESPFileUpdater: %s] Update completed.\n", simpleName);
  } else if (result == ESPFileUpdater::NOT_MODIFIED||result == ESPFileUpdater::MAX_AGE_NOT_REACHED) {
    Serial.printf("[ESPFileUpdater: %s] No update needed.\n", simpleName);
  } else {
    Serial.printf("[ESPFileUpdater: %s] Update failed.\n", simpleName);
  }
}

#ifdef UPDATEURL
  void getRequiredFiles(void* param) {
    for (size_t i = 0; i < requiredFilesCount; i++) {
      player.sendCommand({PR_STOP, 0});
      display.putRequest(NEWMODE, UPDATING);
      const char* fname = requiredFiles[i];
      char localPath[64];
      char remoteUrl[128];
      snprintf(localPath, sizeof(localPath), "/www/%s", fname);
      snprintf(remoteUrl, sizeof(remoteUrl), "%s%s", UPDATEURL, fname);
      updateFile(param, localPath, remoteUrl, "", fname);
    }
    // Delete any files in /www that are not in the requiredFiles list
    File root = SPIFFS.open("/www");
    if (root && root.isDirectory()) {
      File file = root.openNextFile();
      while (file) {
        const char* path = file.name();
        // Extract filename from full path
        const char* name = path;
        const char* slash = strrchr(path, '/');
        if (slash) name = slash + 1;
        bool found = false;
        for (size_t j = 0; j < requiredFilesCount; j++) {
          if (strcmp(name, requiredFiles[j]) == 0) {
            found = true;
            break;
          }
        }
        if (!found) {
          Serial.printf("[File: /www/%s] Deleting - not in required file list.\n", path);
          SPIFFS.remove(path);
        }
        file = root.openNextFile();
      }
    }
  }
#endif //#ifdef UPDATEURL

void startAsyncServices(void* param){
  fixPlaylistFileEnding();
  // if the OTA marker file exists, fetch all web assets immediately, clean up, restart
 #ifdef UPDATEURL
    if (SPIFFS.exists(ONLINEUPDATE_MARKERFILE)) {
      getRequiredFiles(param);
      SPIFFS.remove(ONLINEUPDATE_MARKERFILE);
      delay(200);
      ESP.restart();
    }
  #endif
  updateFile(param, "/www/timezones.json.gz", TIMEZONES_JSON_URL, "1 week", "Timezones database file");
  updateFile(param, "/www/rb_srvrs.json", RADIO_BROWSER_SERVERS_URL, "4 weeks", "Radio Browser Servers list");
  cleanStaleSearchResults();
  vTaskDelete(NULL);
}

void Config::startAsyncServicesButWait() {
  if (WiFi.status() != WL_CONNECTED) return;
  ESPFileUpdater* updater = nullptr;
  updater = new ESPFileUpdater(SPIFFS);
  updater->setMaxSize(1024);
  updater->setUserAgent(ESPFILEUPDATER_USERAGENT);
  xTaskCreate(startAsyncServices, "startAsyncServices", 8192, updater, 2, NULL);
}
