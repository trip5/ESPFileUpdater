# ESPFileUpdater Library

## Overview

The ESPFileUpdater library provides functionality for checking and updating files from a remote server on ESP32/ESP8266 devices. It simplifies the process of managing file updates by handling metadata, calculating file hashes, and ensuring that the local files are up-to-date with the remote versions.

### Features

- Check if a remote file is newer than the local version
- Download updates if available
- Manage metadata files to track last modified time, file hashes and file sizes
- Support for max age checks to prevent unnecessary updates
- Will download even if local file missing
- Verified downloads: a file is only replaced once the transfer is confirmed complete and the installed result is size-checked
- Tolerates slow transfers: a short read is retried until `setReadDeadline()` expires
- Compatible and tested with SPIFFS and LittleFS
- Follows 3xx redirects

### PlatformIO

Your platformio.ini should contain:
```
lib_deps =
  trip5/ESPFileUpdater@^1.6.0
```

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
    ESPFileUpdater::UpdateStatus status = updater.checkAndUpdate("/local/file.txt", "https://example.com/remote/File.txt", "7d", true);

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
If `maxAge` is not specified or is empty, then file information will not be saved to a metadata file (it simply downloads the file).
You may use abbreviations like `hr` or `h` or `d` or `wk` or `w` or `mo` or `m`, with or without spaces, with or without `s`.
Although the process can be called often, it will not update if the accompanying metadata file has a date-stamp within that window of time.
Be reasonable. Don't check for updates too frequently.
If you need to update often, specify `0d` to at least check if the remote file is updated.

`verbose`: use `true` or `false` to enable or disable verbose logging to serial.  It will update every step of the way.  Good for debugging.
If not specified, assumed to be false.

`verbose` only controls the step-by-step `[Info]`, `[Check]`, `[Fallback]`, `[Header]` and `[Heap]` lines.
An `[Error]` that ends an update is printed whether `verbose` is on or off, so a failure does not need a rebuild
to be seen.

You may specify these options in any order, one, both, or not at all.

---

## Process

  - Checks local FS for file existence
    - Waits for FS readiness
      - If not ready 🛑 [stop]
    - If file does not exist ✅ [update]
      - This initial file may get 1970-01-01 as timestamp in its metadata file if system time is incorrect
    - If maxAge not specified ✅ [update]
      - No metadata file is checked, created, or written (if one exists, it will remain)
    - If maxAge is "0" ✅ [update]
      - This initial file may get 1970-01-01 as timestamp in its metadata file if system time is incorrect
  - Waits for system time to be correct
      - If not ready 🛑 [stop]
  - Reads the metadata file URL
    - if URL is different than specified ✅ [update]
  - Validates the metadata file against the file on disk
    - If the size recorded in the metadata file does not match the actual file size, it is stale: the metadata is deleted and the file is re-downloaded ✅ [update]
  - Reads the accompanying metadata file for date
    - If maxAge has not passed then 🛑 [stop]
  - Waits for network connection readiness
    - If not ready 🛑 [stop]
  - Attempts to retrieve date-stamp from remote file
    - If remote file is newer ✅ [update]
    - If remote file is not newer 🛑 [stop]
  - If the server does not support date-stamp, then stream 100KB (at most) from the remote file and generate a hash
    - This hash is compared to the hash stored in the metadata file
    - The response size must be known and no larger than the hashing window, otherwise the hash cannot prove equality ✅ [update]
    - If the sizes differ ✅ [update]
    - If hashes are the same, update the date-stamp in the metadata file with the current date 🛑 [stop]
    - If the hashes differ, assume remote file is newer ✅ [update]

### Update Process

  - Creates every missing directory level of the target path
  - Removes leftover `<path>$` and `<path>~` files from any interrupted attempt, and any `.tmp`, `.bak` or `.meta` file left behind by versions `<1.5.x`
  - Downloads to a temporary `<path>$` file and checks every write
  - The transfer must be complete before anything else happens
    - If `Content-Length` is known, the received byte count must match it exactly
    - For chunked transfers, a terminal zero-length chunk must be received
    - If the response has neither, the body is copied verbatim and a warning is logged (completeness cannot be verified)
    - Truncated, short-written or malformed transfers discard the `<path>$` file and keep the current file 🛑 [retry or stop]
  - Compares the `<path>$` file size on flash to the received byte count
  - Deletes the metadata file `<path>!` before the current file is touched, so it can never describe a missing or partial file
  - Renames the current file to `<path>~`, renames the `<path>$` file into place, then verifies the installed size
    - If any step fails, the `<path>~` file is restored and an error is returned 🛑 [stop]
  - Removes the `<path>~` file only after the new file has been verified
  - Hashes new file (if not already done during stream)
  - Writes the accompanying metadata file `<path>!` last, as the final step

### The Metadata File

The metadata file is the local path with a single `!` appended, so `/www/rb_srvrs.json` is tracked by
`/www/rb_srvrs.json!`. Example content:
```
https://example.com/firmware.bin
1718726400
e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
131072
```

  - The URL the file was last retrieved from
  - UNIX EPOCH of the last update
  - Hash
  - Size of the file in bytes, verified at the time of the update

The metadata file is written only after the downloaded file has been verified and installed,
and it is deleted before the old file is replaced. A metadata file without a size line
(from an older version) is still read, and is upgraded the next time it is written.

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
The same value also becomes the socket timeout, so it bounds a single network read: `Stream::readBytes()` gives up
when it expires. A short read is not the end of the body, so this is not the value that decides whether a transfer
has failed - that is `setReadDeadline()`. Keep it long enough that a single read is never cut short on a slow link.
If you have time-critical operations, check the FreeRTOS_Task folder so that the
updater doesn't block your other operations.  15 seconds may be excessive but safe.

`setUserAgent(const String& ua);` Some developers might like to know who / what program is fetching data from them.

`setInsecure(bool insecure);` Enables insecure mode.  It will disable checking of secure certificates when using HTTPS connections. This may help lower memory use (not much likely).  Setting to `true` seems to make connections fail more often.

`setBuffer(size_t bytes)` Sets the buffer size.  Bigger is faster.

`setYieldInterval(milliseconds)` Sets after how many ms, `yield();` is executed.  Can help responsiveness. Diminishing returns below `10`. Use `0` if other processes need to be greedy.

`setReadDeadline(uint32_t ms)` How long the library will wait inside one chunk before calling the body truncated.  Default `15000`.
It bounds the total waiting within that chunk rather than any single read, so a slow connection is given time to
finish while a server that has genuinely stopped still ends the transfer.  Set it well above the slowest believable gap.

`setRetryCount(uint8_t count)` Number of retry attempts on transient network failures. Each retry waits `retryDelay` ms.

`setRetryDelay(uint32_t ms)` Delay between retry attempts in milliseconds.

#### Stack Allocation (with defaults)

```cpp
  ESPFileUpdater updater(SPIFFS);
  updater.setMaxSize(102400);      // 100 KB max stream size for hashing
  updater.setTimeout(15000);       // 15000ms / 15s for timeout (for each check)
  updater.setUserAgent("MyESPProject/1.2.3 (https://github.com/me/MyESPProject)");
  updater.setInsecure(false);      // insecure mode enabled
  updater.setBuffer(2048);
  updater.setYieldInterval(20);
  updater.setReadDeadline(15000);  // 15s total wait within one chunk
  updater.setRetryCount(2);
  updater.setRetryDelay(2000);
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
  updater->setReadDeadline(15000);  // 15s total wait within one chunk
  updater->setRetryCount(2);
  updater->setRetryDelay(2000);
```

### Memory and the TLS Handshake

Before each TLS connection the library asks the allocator that mbedTLS will actually use, instead of comparing
`ESP.getFreeHeap()` against a fixed floor:

```cpp
void* in  = mbedtls_calloc(1, 16384);   // TLS record buffer, in
void* out = mbedtls_calloc(1, 16384);   // TLS record buffer, out
void* ctx = mbedtls_calloc(1, 8192);    // handshake context plus margin
ok = in && out && ctx;                  // held at once: fragmentation is what fails a handshake
```

- The three blocks are held simultaneously because a handshake needs them simultaneously, then freed at once.
  Total free bytes say nothing about the largest contiguous block, and a handshake fails on the largest block.
- This is correct whichever pool mbedTLS was built to use.  Arduino ships it on internal DRAM
  (`CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC`), while an mbedTLS rebuilt with `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` takes
  the same buffers from PSRAM.  A version check cannot see a rebuilt archive; asking the allocator can.
- On a board without PSRAM the probe simply lands in internal DRAM, which is where TLS will allocate as well.  No
  PSRAM requirement is introduced.
- Guarded by `#if defined(MBEDTLS_PLATFORM_MEMORY)`; where that is not defined the original total-heap test is
  compiled unchanged.
- When the probe fails, `OUT_OF_MEMORY` is returned and the buffer that could not be allocated is named in the log,
  without needing `verbose`.

The two anti-fragmentation reserves are separate and unchanged: a `malloc(40000)` is held through `HTTPClient`
setup and freed immediately before the handshake.

### UpdateStatus Enum

- **UPDATED**: Indicates that the file was updated successfully.
- **MAX_AGE_NOT_REACHED**: Indicates that the maximum age for updates has not been reached.
- **NOT_MODIFIED**: Indicates that the remote file has not been modified.
- **SERVER_ERROR**: Indicates that there was an error connecting to the server.
- **FILE_NOT_FOUND**: Indicates that the remote file was not found.
- **FS_ERROR**: Indicates an error with the file system (SPIFFS or LittleFS).
- **TIME_ERROR**: Indicates that system time was not set.
- **NETWORK_ERROR**: Indicates the network connect was not ready.
- **CONNECTION_FAILED**: Indicates a connection error; error returned by the upstream library will be shown if verbose is on.
- **OUT_OF_MEMORY**: Indicates insufficient heap memory for the operation (e.g., SSL connection or buffer allocation failed).

---

## File System Limitations

This library derives three names from the path you hand it, and each has to fit the file system. Every one of
them adds a **single character**, so there is one budget to remember rather than one per name:

| Derived name | Extra characters | When it exists |
| --- | --- | --- |
| `<path>$` | 1 | during the download, removed after the verified swap |
| `<path>~` | 1 | during the swap only, removed once the new file is verified |
| `<path>!` | 1 | one per tracked file, and only when `maxAge` is used |

Two consequences of single characters are worth knowing:

- A file of your own whose name ends in `!`, `$` or `~` can collide with a derived name, so avoid ending your own
  names with those three characters.
- A web file list or file browser that treated `.meta` as a dotfile now shows these as ordinary names. Filter names
  ending in `!`, `$` or `~` if you list the directory.

### SPIFFS

- A name **including the whole path** is limited to **31 characters** (`SPIFFS_OBJ_NAME_LEN` is 32 including the
  terminator), so `/www/` spends 5 of them.
- One character goes to the derived name, so the real usable length is **30 characters**.

### LittleFS on ESP8266

Taken from the ESP8266 Arduino core 3.1.2 sources, not from guesses:

- `libraries/LittleFS/src/LittleFS.h` forces `#define LFS_NAME_MAX 32` before including littlefs, and the mount
  sets `name_max = 0`, so littlefs falls back to that 32 (`lib/littlefs/lfs.c`).  The usable limit is
  **31 characters**: the same number as SPIFFS.
- It is counted **per path component**, not for the whole path.  Only the final name is measured, so
  `/deep/path/to/` costs nothing.  That is the sole improvement over SPIFFS.
- It cannot be raised from a sketch: the value is compiled into the core and also recorded in the littlefs
  superblock.
- So the usable length is again **30 characters**.

### LittleFS on ESP32

- The ESP32 core uses esp_littlefs, whose `CONFIG_LITTLEFS_OBJ_NAME_LEN` defaults to **64**, giving **63 usable
  characters** per name, or **62** once the derived character is added.
- It is configurable in a custom core build, so the number in *your* build is the one that counts.  A name that does
  not fit fails at open time, and the error names the file it could not open.

### What happens when a name does not fit

- `<path>$` is opened first, so a name that fits for the file but not once the derived character is added fails
  the download with `FS_ERROR`, leaving the current file untouched.
- If only `<path>!` does not fit, the file itself updates correctly and no metadata is written.  With no
  date-stamp to compare, **every later call re-downloads the file**.  That failure is logged as an `[Error]`
  whether `verbose` is on or off.
- The error names the file it could not open, so an over-long name is identifiable from the log without `verbose`.

### Directories

- SPIFFS has no real directories - a path is only part of the name - so `/www/file` needs no setup.
- LittleFS keeps real directories.  The library creates every missing level of the target path, and returns
  `FS_ERROR` if one cannot be created.
- ESP8266 `LittleFS.open(..., "w")` also creates missing parents on its own; esp_littlefs on ESP32 does not,
  unless `CONFIG_LITTLEFS_SPIFFS_COMPAT` is enabled.  That is why the library creates them explicitly.

### Free space

- Both file systems allocate in whole blocks (4 KB for esp_littlefs, several KB on ESP8266), so a ~150 byte
  metadata file costs a whole block, and littlefs also needs free blocks for its own metadata updates.
- The `<path>$` duplicate means the peak requirement is roughly **twice the file size plus the metadata file**.
  A file system that is nearly full returns `FS_ERROR` rather than a truncated file.

---

## Examples

Check the `examples` folder for examples of how to use the ESPFileUpdater library in your projects.

---

## Update History

| Date       | Version | Release Notes             |
| ---------- | ------- |-------------------------- |
| 2026.11.12 | 1.6.0   | Allocator-based TLS heap probe, tolerant chunk reads, always-on errors, one-character file suffixes, recursive directories |
| 2026.10.19 | 1.5.0   | Verified downloads: incomplete transfers are rejected, the swap is size-checked with a .bak rollback, and the .meta file is written only after the swap and records the file size |
| 2026.08.10 | 1.4.2   | Add heap coalesce and reserve so SSL connections always get 40KB contiguous DRAM |
| 2026.06.29 | 1.4.1   | Fixed edge-case of non-chunked binary file corruption |
| 2026.06.26 | 1.4.0   | Retry loop no longer retries on definitive answers |
| 2026.06.17 | 1.3.0   | Added OUT_OF_MEMORY status, heap check before SSL connections, retry logic, nothrow buffer allocation |
| 2026.02.08 | 1.2.0   | Handles chunked transfers properly |
| 2026.02.04 | 1.1.2   | PlatformIO examples and readme fixed |
| 2026.02.03 | 1.1.1   | PlatformIO release        |
| 2025.07.20 | 1.1.0   | Settings added            |
| 2025.06.29 | 1.0.0   | First release             |

---

## License

This library is released under the MIT License.

See [LICENSE](LICENSE) for details.

