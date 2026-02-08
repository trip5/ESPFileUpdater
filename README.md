# ESPFileUpdater Library

## Overview
The ESPFileUpdater library provides functionality for checking and updating files from a remote server on ESP32/ESP8266 devices. It simplifies the process of managing file updates by handling metadata, calculating file hashes, and ensuring that the local files are up-to-date with the remote versions.

### Features
- Check if a remote file is newer than the local version
- Download updates if available
- Manage metadata files to track last modified time and file hashes
- Support for max age checks to prevent unnecessary updates
- Will download even if local file missing
- Compatible with SPIFFS (tested) and LittleFS (untested)
- Follows 3xx redirects

### PlatformIO
Your platformio.ini should contain:
```
lib_deps =
  trip5/ESPFileUpdater@^1.2.0
```

### Installation
To install the ESPFileUpdater library, follow these steps:
1. Download the library from the repository.
2. Extract the contents to your Arduino libraries folder (usually located in `Documents/Arduino/libraries`).
3. Restart the Arduino IDE to recognize the new library.

### Dependent on Internet, File System, and System Time
- It depends on an Internet connection, file system, and system time.
- Only run it after the these 3 process are stable (usually).
- If system time is not available at the time it runs, it will download the file only if it does not already exist on the file system.

---

## Usage
To use the ESPFileUpdater library in your project, include the header file and create an instance of the `ESPFileUpdater` class. Here is a basic example:

```cpp
#include <WiFi.h>
#include <SPIFFS.h>
#include <ESPFileUpdater.h>

// WiFi credentials
const char* ssid = "YOUR_SSID";
const char* password = "YOUR_PASSWORD";

void setup() {
    Serial.begin(115200);
    SPIFFS.begin(true);
    WiFi.begin(ssid, password);
    
    // Wait for connection
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("WiFi connected!");

    // Set system time via SNTP
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Waiting for NTP time sync");
    time_t now = time(nullptr);
    while (now < 8 * 3600 * 2) {
        delay(500);
        Serial.print(".");
        now = time(nullptr);
    }
    Serial.println(" Time synchronized!");

    ESPFileUpdater updater(SPIFFS);
    updater.setTimeout(5000);
    ESPFileUpdater::UpdateStatus status = updater.checkAndUpdate("/path/to/local/file.txt", "https://example.com/remote/File.txt", "7d", true);

    if (status == ESPFileUpdater::UPDATED) {
        Serial.println("[ESPFileUpdater: File.txt] Update completed.");
    } else if (status == ESPFileUpdater::NOT_MODIFIED || status == ESPFileUpdater::MAX_AGE_NOT_REACHED) {
        Serial.println("[ESPFileUpdater: File.txt] No update needed.");
    } else {
        Serial.println("[ESPFileUpdater: File.txt] Update failed.");
    }
}

void loop() {
    // Your main code here
}
```
---
## Syntax

`ESPFileUpdater::UpdateStatus status = updater.checkAndUpdate("/local/file", "https://remote/file", "maxAge", verbose);`

### Mandatory

`/local/file` : path and file on the file system - probably best to start with root `/`.

`https://remote/file` : try to use a URL that doesn't do a re-direct - see the FreeRTOS_Task example for a Github retrieval.

### Options

`maxAge`: can accept `X hours`, `X days`, `X weeks`, `X months` as an argument.
If the file does not exist or `maxAge` is not specified, it will download/update immediately, without any checks.
If `maxAge` is not specified or is empty, then file information will not be saved to a .meta file (it simply downloads the file).
You may use abbreviations like `hr` or `h` or `d` or `wk` or `w` or `mo` or `m`, with or without spaces, with or without `s`.
Although the process can be called often, it will not update if the accompanying .meta file has a date-stamp within that window of time.
Be reasonable. Don't check for updates too frequently.
If you need to update often, specify `0d` to at least check if the remote file is updated.

`verbose`: use `true` or `false` to enable or disable verbose logging to serial.  It will update every step of the way.  Good for debugging.
If not specified, assumed to be false.

You may specify these options in any order, one, both, or not at all.

---

## Process
- Checks local FS for file existence
  - Waits for FS readiness
    - If not ready 🛑 [stop]
  - If file does not exist ✅ [update]
    - This initial file may get 1970-01-01 as timestamp in .meta file if system time is incorrect
  - If maxAge not specified ✅ [update]
    - No .meta file is checked, created, or written (if one exists, it will remain)
  - If maxAge is "0" ✅ [update]
    - This initial file may get 1970-01-01 as timestamp in .meta file if system time is incorrect
- Waits for system time to be correct
    - If not ready 🛑 [stop]
- Reads .meta file URL
  - if URL is different than specified ✅ [update]
- Reads accompanying .meta file for date
  - If maxAge has not passed then 🛑 [stop]
- Waits for network connection readiness
    - If not ready 🛑 [stop]
- Attempts to retrieve date-stamp from remote file
  - If remote file is newer ✅ [update]
  - If remote file is not newer 🛑 [stop]
- If the server does not support date-stamp, then stream 100KB (at most) from the remote file and generate a hash
  - This hash is compared to a hash stored in the .meta file
  - If hashes are the same, update the date-stamp in the meta file with the current date 🛑 [stop]
  - If the hashes differ, assume remote file is newer ✅ [update]

### Update Process

- Downloads to temporary file first for safety
- Deletes old file, renames new file
- Hashes new file (if not already done during stream)
- Writes accompanying .meta file

### The .meta File

Example
```
https://example.com/firmware.bin
1718726400
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
```

- The URL the file was last retrieved from
- UNIX EPOCH of the last update
- Hash

---

## API Reference

### ESPFileUpdater Class
- **ESPFileUpdater(fs::FS& fs)**: Constructor that initializes the updater with the specified file system.
- **UpdateStatus checkAndUpdate(const String& localPath, const String& remoteURL, const String& maxAge)**: Checks if the remote file is newer and updates if necessary.

### Settings

These can be changed.

`setMaxSize(size_t bytes);` If the date comparison fails, the file will be streamed from the server (without downloading) and compared to the file on the system. This helps reduce how many bytes are needed to generate a hash for the comparison.
100KB seems a reasonable default but you can increase or decrease the number.

`setTimeout(uint32_t ms);` You should always do your best to make sure the various components needed for
this are ready but there are checks and timeouts as part of the routines to wait if they are ready.
If you have time-critical operations, check the FreeRTOS_Task folder so that the
updater doesn't block your other operations.  15 seconds is excessive I'm sure but it's just in case.

`setUserAgent(const String& ua);` Some developers might like to know who / what program is fetching data from them.

`setInsecure(bool insecure);` Enables insecure mode.  It will disable checking of secure certificates when using HTTPS connections. This may help lower memory use (not much likely).  Setting to `true` seems to make connections fail more often.

`setBuffer(size_t bytes)` Sets the buffer size.  Bigger is faster.

`setYieldInterval(milliseconds)` Sets after how many ms, `yield();` is executed.  Can help responsiveness. Diminishing returns below `10`. Use `0` if other processes need to be greedy.

#### Stack Allocation (with defaults)

```cpp
  ESPFileUpdater updater(SPIFFS);
  updater.setMaxSize(102400);      // 100 KB max stream size for hashing
  updater.setTimeout(15000);       // 15000ms / 15s for timeout (for each check)
  updater.setUserAgent("MyESPProject/1.2.3 (https://github.com/me/MyESPProject)");
  updater.setInsecure(false);      // insecure mode enabled
  updater.setBuffer(2048);
  updater.setYieldInterval(20);
```

#### Heap Allocation

```cpp
  ESPFileUpdater* updater = nullptr;
  updater = new ESPFileUpdater(SPIFFS);
  updater->setMaxSize(102400);
  updater->setTimeout(15000);
  updater->setUserAgent("MyESPProject/1.2.3 (https://github.com/me/MyESPProject)");
  updater->setInsecure(false);
  updater->setBuffer(2048);
  updater->setYieldInterval(20);
```

### UpdateStatus Enum

- **UPDATED**: Indicates that the file was updated successfully.
- **MAX_AGE_NOT_REACHED**: Indicates that the maximum age for updates has not been reached.
- **NOT_MODIFIED**: Indicates that the remote file has not been modified.
- **SERVER_ERROR**: Indicates that there was an error connecting to the server.
- **FILE_NOT_FOUND**: Indicates that the remote file was not found.
- **FS_ERROR**: Indicates an error with the SPIFFS file system.
- **TIME_ERROR**: Indicates that system time was not set.
- **NETWORK_ERROR**: Indicates the network connect was not ready.
- **CONNECTION_FAILED**: Indicates a connection error; error returned by the upstream library will be shown if verbose is on.
---

## SPIFFS Limitations

Keep in mind file names (including path) must be 31 characters or less.
Because of this limitation and the fact this library adds
extra extensions to the .meta and .tmp download files,
the real usable file name length limit is 27 characters!

I'm not sure about LittleFS limitations.

---

## Examples

Check the `examples` folder for examples of how to use the ESPFileUpdater library in your projects.

---

## Update History

| Date       | Version | Release Notes             |
| ---------- | ------- |-------------------------- |
| 2026.02.08 | 1.2.0   | Handles chunked transfers properly |
| 2026.02.04 | 1.1.2   | PlatformIO examples and readme fixed |
| 2026.02.03 | 1.1.1   | PlatformIO release        |
| 2025.07.20 | 1.1.0   | Settings added            |
| 2025.06.29 | 1.0.0   | First release             |

---

## License

This library is released under the MIT License. See the LICENSE file for more details.