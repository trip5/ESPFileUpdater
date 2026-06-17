#include <ESPFileUpdater.h>

// This example is adapted from ehRadio
// See: https://github.com/trip5/ehRadio

// List of WebUI asset files — matches src/core/config.cpp Config::wwwFiles[]
static const char* requiredFiles[] = {"curated.js", "options.js", "script.js", "script2.js", "search.js",
                                      "logo.svg", "icon.png", "locales.json", "rb_srvrs.json", "timezones.json",
                                      "style.css", "theme.css",
                                      "curated.html", "irrecord.html", "options.html", "search.html", "updform.html",
                                      "player.html"};
static const size_t requiredFilesCount = sizeof(requiredFiles) / sizeof(requiredFiles[0]);

bool wasUpdated(ESPFileUpdater::UpdateStatus status) { return status == ESPFileUpdater::UPDATED; }

// Download all required web files at boot (called when wwwFilesExist == false)
#ifdef FILESURL
  void getRequiredFiles(void* param) {
    ESPFileUpdater* updater = (ESPFileUpdater*)param;
    updater->setMaxSize(1024);
    updater->setUserAgent("ESPFileUpdater/1.3.0 (ehRadio)");
    updater->setRetryCount(2);
    updater->setRetryDelay(2000);

    char localFileGz[64];
    char localFile[64];
    char tryFile[64];
    char tryUrl[128];
    for (size_t i = 0; i < requiredFilesCount; i++) {
      const char* fname = requiredFiles[i];
      snprintf(localFileGz, sizeof(localFileGz), "/www/%s.gz", fname);
      snprintf(localFile, sizeof(localFile), "/www/%s", fname);
      if (SPIFFS.exists(localFileGz)) SPIFFS.remove(localFileGz);
      if (SPIFFS.exists(localFile)) SPIFFS.remove(localFile);
      for (size_t j = 0; j < 2; j++) {
        if (j == 0) { // Try compressed first
          snprintf(tryFile, sizeof(tryFile), "%s", localFileGz);
          snprintf(tryUrl, sizeof(tryUrl), "%s%s.gz", FILESURL, fname);
        } else { // Fallback to uncompressed
          snprintf(tryFile, sizeof(tryFile), "%s", localFile);
          snprintf(tryUrl, sizeof(tryUrl), "%s%s", FILESURL, fname);
        }
        Serial.printf("[ESPFileUpdater: %s] Updating required file.\n", tryFile);
        ESPFileUpdater::UpdateStatus result = updater->checkAndUpdate(
            tryFile,
            tryUrl,
            "",
            true
        );
        if (result == ESPFileUpdater::UPDATED) {
          Serial.printf("[ESPFileUpdater: %s] Download completed.\n", tryFile);
          break;
        } else {
          if (j == 0) Serial.printf("[ESPFileUpdater: %s] Download failed. Retrying uncompressed.\n", tryFile);
          if (j == 1) Serial.printf("[ESPFileUpdater: %s] Download failed.\n", tryFile);
        }
      }
    }
    delay(200);
    ESP.restart();
    vTaskDelete(NULL);
  }
#endif

// Background update task — checks for newer versions of data files
void startAsyncServices(void* param) {
  // Wait for audio stream to stabilize before using network bandwidth
  vTaskDelay(pdMS_TO_TICKS(10000));

  ESPFileUpdater* updater = (ESPFileUpdater*)param;
  updater->setMaxSize(1024);
  updater->setUserAgent("ESPFileUpdater/1.3.0 (ehRadio)");
  updater->setRetryCount(2);
  updater->setRetryDelay(2000);

  // Update timezone database (weekly check)
  ESPFileUpdater::UpdateStatus result = updater->checkAndUpdate(
    "/www/timezones.json.gz",
    "https://raw.githubusercontent.com/trip5/timezones.json/master/timezones.json.gz",
    "1 week",
    true
  );
  if (result == ESPFileUpdater::UPDATED) {
    Serial.println("[ESPFileUpdater] Timezones updated");
  }

  // Update Radio-Browser servers list (daily check)
  result = updater->checkAndUpdate(
    "/www/rb_srvrs.json",
    "https://all.api.radio-browser.info/json/servers",
    "1 day",
    true
  );
  if (result == ESPFileUpdater::UPDATED) {
    Serial.println("[ESPFileUpdater] RB Servers updated");
  }

  delete updater;
  vTaskDelete(NULL);
}

void setup() {
  Serial.begin(115200);
  SPIFFS.begin(true);
  WiFi.begin("SSID", "PASSWORD");
  while (WiFi.status() != WL_CONNECTED) delay(500);

  configTime(0, 0, "pool.ntp.org");
  time_t now = time(nullptr);
  while (now < 100000) { delay(100); now = time(nullptr); }

  // Start background updater with heap checking and retry support
  ESPFileUpdater* updater = new ESPFileUpdater(SPIFFS);
  xTaskCreatePinnedToCore(startAsyncServices, "startAsyncServices", 8192, updater, 1, NULL, 1);
}

void loop() {}
