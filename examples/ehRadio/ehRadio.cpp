#include <ESPFileUpdater.h>

// This example from ehRadio

// List of required web asset files
static const char* requiredFiles[] = {"dragpl.js","ir.css","irrecord.html","ir.js","logo.svg","options.html","script.js",
                                     "timezones.json","search.html","search.js","search.css", //"rb_srvrs.json" is optional
                                     "style.css","updform.html","theme.css","player.html"}; // keep main page at end
static const size_t requiredFilesCount = sizeof(requiredFiles) / sizeof(requiredFiles[0]);

bool wasUpdated(ESPFileUpdater::UpdateStatus status) { return status == ESPFileUpdater::UPDATED; }

// 

#ifdef UPDATEURL
  void getRequiredFiles(void* param) {
    player.sendCommand({PR_STOP, 0});
    char localFileGz[64];
    char localFile[64];
    char tryFile[64];
    char tryUrl[128];
    for (size_t i = 0; i < requiredFilesCount; i++) {
      display.putRequest(NEWMODE, UPDATING);
      const char* fname = requiredFiles[i];
      snprintf(localFileGz, sizeof(localFileGz), "/www/%s.gz", fname);
      snprintf(localFile, sizeof(localFile), "/www/%s", fname);
      if (SPIFFS.exists(localFileGz)) SPIFFS.remove(localFileGz);
      if (SPIFFS.exists(localFile)) SPIFFS.remove(localFile);
      for (size_t j = 0; j < 2; j++) {
        if (j == 0) { // Try compressed first
          snprintf(tryFile, sizeof(tryFile), "%s", localFileGz);
          snprintf(tryUrl, sizeof(tryUrl), "%s%s.gz", UPDATEURL, fname);
        } else { // Fallback to uncompressed
          snprintf(tryFile, sizeof(tryFile), "%s", localFile);
          snprintf(tryUrl, sizeof(tryUrl), "%s%s", UPDATEURL, fname);
        }
        Serial.printf("[ESPFileUpdater: %s] Updating required file.\n", tryFile);
        ESPFileUpdater* getfile = (ESPFileUpdater*)param;
        ESPFileUpdater::UpdateStatus result = getfile->checkAndUpdate(
            tryFile,
            tryUrl,
            "",
            ESPFILEUPDATER_VERBOSE
        );
        if (result == ESPFileUpdater::UPDATED) {
          Serial.printf("[ESPFileUpdater: %s] Download completed.\n", tryFile);
          break; // Exit inner loop on success
        } else {
          if (j == 0) Serial.printf("[ESPFileUpdater: %s] Download failed. Will retry for uncompressed file.\n", tryFile);
          if (j == 1) Serial.printf("[ESPFileUpdater: %s] Download failed. Moving onto next file anyways.\n", tryFile);
        }
      }
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
          // Check against both compressed and uncompressed names
          char requiredNameGz[64];
          snprintf(requiredNameGz, sizeof(requiredNameGz), "%s.gz", requiredFiles[j]);
          if (strcmp(name, requiredFiles[j]) == 0 || strcmp(name, requiredNameGz) == 0) {
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
    delay(200);
    ESP.restart();
    vTaskDelete(NULL);
  }
#endif //#ifdef UPDATEURL

void Config::updateFile(void* param, const char* localFile, const char* onlineFile, const char* updatePeriod, const char* simpleName) {
  Serial.printf("[ESPFileUpdater: %s] Started update.\n", simpleName);
  ESPFileUpdater* updatefile = (ESPFileUpdater*)param;
  ESPFileUpdater::UpdateStatus result = updatefile->checkAndUpdate(
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

void startAsyncServices(void* param) {
  fixPlaylistFileEnding();
  config.updateFile(param, "/www/timezones.json.gz", TIMEZONES_JSON_URL, "1 week", "Timezones database file");
  config.updateFile(param, "/www/rb_srvrs.json", RADIO_BROWSER_SERVERS_URL, "4 weeks", "Radio Browser Servers list");
  cleanStaleSearchResults();
  vTaskDelete(NULL);
}

void Config::startAsyncServicesButWait() {
  if (WiFi.status() != WL_CONNECTED) return;
  ESPFileUpdater* updater = nullptr;
  updater = new ESPFileUpdater(SPIFFS);
  updater->setMaxSize(1024);
  updater->setUserAgent(ESPFILEUPDATER_USERAGENT);
  if (emptyFS) {
    #ifdef UPDATEURL
      xTaskCreate(getRequiredFiles, "getRequiredFiles", 8192, updater, 2, NULL);
    #endif
  } else {
    xTaskCreate(startAsyncServices, "startAsyncServices", 8192, updater, 2, NULL);
  }
}