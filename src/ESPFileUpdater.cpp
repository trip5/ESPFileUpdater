// ESPFileUpdater 1.5.0 -- Created by Trip5 : https://github.com/trip5/ESPFileUpdater

#include "ESPFileUpdater.h"
#include <time.h>
#include <new>
#include <WiFiClient.h>
#include <Arduino.h>
#include <lwip/sockets.h>
#include <lwip/netdb.h>
#include <WiFi.h>

// Compatibility for mbedtls_sha256_*_ret functions (for older ESP32 Arduino cores)
#if !defined(mbedtls_sha256_starts_ret)
#define mbedtls_sha256_starts_ret mbedtls_sha256_starts
#define mbedtls_sha256_update_ret mbedtls_sha256_update
#define mbedtls_sha256_finish_ret mbedtls_sha256_finish
#endif

// Minimum free heap required for SSL/TLS connections (in bytes)
// mbedtls needs ~34-36KB contiguous (16KB in + 16KB out + context). 40KB gives safe margin.
#define ESPFILEUPDATER_MIN_HEAP_FOR_SSL 40000

// Largest chunk size accepted while parsing chunked transfer framing (1 MB)
#define ESPFILEUPDATER_MAX_CHUNK_SIZE 1048576

ESPFileUpdater::ESPFileUpdater(fs::FS& fs) : _fs(fs) {}

// Convert a raw SHA256 digest into a lowercase hex string.
static String hashToString(const uint8_t* hash) {
  String result;
  for (int i = 0; i < 32; i++) {
    if (hash[i] < 16) result += "0";
    result += String(hash[i], HEX);
  }
  return result;
}

// Yield only once the configured interval has elapsed, keeping other tasks responsive.
static void yieldIfDue(uint32_t& lastYieldTime, uint32_t interval) {
  if (millis() - lastYieldTime >= interval) {
    yield();
    lastYieldTime = millis();
  }
}

// Check if sufficient heap is available for an SSL connection.
// Returns true if OK, false if heap critically low.
// NOTE: The caller is responsible for holding a contiguous reserve block
// (malloc + hold through setup + free immediately before SSL handshake)
// to prevent the audio task from fragmenting the heap between check and use.
static bool checkHeapForSSL(const String& localPath, bool verbose) {
    uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < ESPFILEUPDATER_MIN_HEAP_FOR_SSL) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Insufficient heap (%u bytes, need %d). Aborting.\n",
            localPath.c_str(), (unsigned)freeHeap, ESPFILEUPDATER_MIN_HEAP_FOR_SSL);
        return false;
    }
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Heap] Free heap: %u bytes\n", localPath.c_str(), (unsigned)freeHeap);
    return true;
}

/// @copydoc ESPFileUpdater::checkAndUpdate(const String&, const String&, const String&, bool)
ESPFileUpdater::UpdateStatus ESPFileUpdater::checkAndUpdate(const String& localPath, const String& remoteURL, const String& maxAge, bool verbose) {
  String meta = metaPath(localPath);
  bool ForceUpdate = false;
  String newLastModified = "";
  String newHash = "";

  if(_insecure) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Insecure mode enabled: disable checking of secure certificates.\n", localPath.c_str());
  }

  if (waitForSystemReadyFS()==false) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] File system not ready. Aborting update.\n", localPath.c_str());
    return FS_ERROR;
  }

  if (!_fs.exists(localPath)) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] File doesn't exist. Forcing download.\n", localPath.c_str());
    ForceUpdate = true;
  } else if (maxAge == "" || maxAge == "0") {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Downloading.\n", localPath.c_str());
    ForceUpdate = true;
  } else {
    if (waitForSystemReadyTime()==false) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] System time not set. Aborting update.", localPath.c_str());
      return TIME_ERROR;
    }
    // check .meta file if URL has changed
    if (_fs.exists(meta)) {
      String storedURL = readMetaURL(meta);
      if (storedURL.length() > 0 && storedURL != remoteURL) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Source URL changed. Forcing re-download.\n", localPath.c_str());
        ForceUpdate = true;
      } else {
        // A .meta file must never outlive the file it describes. If the recorded size
        // no longer matches the file on disk, the .meta is stale and is discarded.
        String storedSize = readMetaSize(meta);
        if (storedSize.length() > 0) {
          File localFile = _fs.open(localPath, FILE_READ);
          bool haveLocalFile = (bool)localFile;
          size_t localSize = haveLocalFile ? localFile.size() : 0;
          if (haveLocalFile) localFile.close();
          unsigned long metaSize = strtoul(storedSize.c_str(), NULL, 10);
          if (!haveLocalFile || (size_t)metaSize != localSize) {
            if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] .meta size (%lu) does not match local size (%u). Discarding .meta and forcing re-download.\n",
                                        localPath.c_str(), metaSize, (unsigned)localSize);
            _fs.remove(meta);
            ForceUpdate = true;
          }
        }
      }
    }
  }

  if (ForceUpdate == false) {
    // check .meta file and maxAge
    time_t now = time(nullptr);
    if (maxAge.length() > 0 && _fs.exists(meta)) {
      time_t metaTime = parseMetaTime(meta);
      time_t interval = parseMaxAge(maxAge);

      char nowStr[32];
      strftime(nowStr, sizeof(nowStr), "%Y-%m-%d %H:%M:%S", localtime(&now));
      if (verbose) Serial.printf("[ESPFileUpdater: %s] Current time: %s\n", localPath.c_str(), nowStr);
      char metaTimeStr[32];
      strftime(metaTimeStr, sizeof(metaTimeStr), "%Y-%m-%d %H:%M:%S", localtime(&metaTime));
      if (verbose) Serial.printf("[ESPFileUpdater: %s] .meta file time: %s\n", localPath.c_str(), metaTimeStr);

      if (metaTime > 0 && interval > 0 && (now < metaTime + interval)) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Skipping update: max age (%s) not reached.\n", localPath.c_str(), maxAge.c_str());
        return MAX_AGE_NOT_REACHED;
      }
    }

    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Check] Connecting to %s\n", localPath.c_str(), remoteURL.c_str());
    if (waitForSystemReadyNetwork()==false) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Network connection not ready. Aborting.\n", localPath.c_str());
      return NETWORK_ERROR;
    }

    // Check heap before attempting SSL connection
    if (!checkHeapForSSL(localPath, verbose)) {
        return OUT_OF_MEMORY;
    }

    // Retry loop for transient failures.
    // Each iteration allocates a contiguous heap reserve, holds it through
    // HTTPClient setup to prevent another task from fragmenting DRAM, and
    // frees it immediately before the TLS handshake.
    for (uint8_t attempt = 0; attempt <= _retryCount; attempt++) {
      if (attempt > 0) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Retry] Attempt %d of %d\n", localPath.c_str(), attempt, _retryCount);
        delay(_retryDelay);
        // Recheck heap before each retry
        if (!checkHeapForSSL(localPath, verbose)) {
            return OUT_OF_MEMORY;
        }
      }

    // Allocate contiguous heap reserve to prevent fragmentation
    void* sslReserve = malloc(ESPFILEUPDATER_MIN_HEAP_FOR_SSL);
    if (!sslReserve) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Failed to reserve %d bytes for SSL (heap: %u)\n",
          localPath.c_str(), ESPFILEUPDATER_MIN_HEAP_FOR_SSL, (unsigned)ESP.getFreeHeap());
      return OUT_OF_MEMORY;
    }

    WiFiClientSecure secureClient;
    HTTPClient http;

    if (_insecure) {
      secureClient.setInsecure();
      http.begin(secureClient, remoteURL);
    } else {
      http.begin(remoteURL);
    }

    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent(_userAgent);

    // Free reserve immediately before TLS handshake so mbedtls can claim it
    free(sslReserve);

    int httpCode = http.sendRequest("HEAD");

    if (httpCode <= 0) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Server unreachable.\n", localPath.c_str());
      http.end();
      if (attempt < _retryCount) continue; // retry on transient failure
      return SERVER_ERROR;
    }

    String lastModified = "";
    int32_t remoteSize = -1;
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Response] HTTP code: %d\n", localPath.c_str(), httpCode);
    if (httpCode == HTTP_CODE_NOT_FOUND) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] File not found on server. Trying GET anyways.\n", localPath.c_str());
    } else {
      lastModified = http.header("Last-Modified");
      remoteSize = (int32_t)http.getSize();
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Header] Last-Modified: %s\n", localPath.c_str(), lastModified.c_str());
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Header] Content-Length: %d\n", localPath.c_str(), (int)remoteSize);
    }

    UpdateStatus status = isRemoteFileNewer(localPath, remoteURL, lastModified, remoteSize, newLastModified, newHash, verbose);

    if (status != UPDATED) {
      if (status == NOT_MODIFIED) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Complete] Remote file is same.\n", localPath.c_str());
        http.end();
        return NOT_MODIFIED; // definitive answer, no retry needed
      }
      if (status == FILE_NOT_FOUND) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] File not found. Aborting.\n", localPath.c_str());
        http.end();
        return FILE_NOT_FOUND; // definitive answer, no retry needed
      }
      if (status == SERVER_ERROR) {
        if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Server error. Aborting.\n", localPath.c_str());
        http.end();
        if (attempt < _retryCount) continue; // retry on transient failure
        return SERVER_ERROR;
      }
      http.end();
      if (attempt < _retryCount) continue; // retry on transient failure
      return status;
    }

    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Update] Downloading remote file...\n", localPath.c_str());
    http.end();
    break; // success, exit retry loop
  } // end retry loop
  } // end else (ForceUpdate == false)

  if (waitForSystemReadyNetwork()==false) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Network connection not ready. Aborting.\n", localPath.c_str());
    return NETWORK_ERROR;
  }

  // Check heap before second SSL connection (download)
  if (!checkHeapForSSL(localPath, verbose)) {
    return OUT_OF_MEMORY;
  }

  // Retry loop for download phase.
  // Each iteration allocates a contiguous heap reserve, holds it through
  // HTTPClient setup to prevent another task from fragmenting DRAM, and
  // frees it immediately before the TLS handshake.
  for (uint8_t attempt = 0; attempt <= _retryCount; attempt++) {
    if (attempt > 0) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Retry] Download attempt %d of %d\n", localPath.c_str(), attempt, _retryCount);
      delay(_retryDelay);
      if (!checkHeapForSSL(localPath, verbose)) {
        return OUT_OF_MEMORY;
      }
    }

  // Allocate contiguous heap reserve to prevent fragmentation
  void* sslReserve = malloc(ESPFILEUPDATER_MIN_HEAP_FOR_SSL);
  if (!sslReserve) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Failed to reserve %d bytes for SSL (heap: %u)\n",
        localPath.c_str(), ESPFILEUPDATER_MIN_HEAP_FOR_SSL, (unsigned)ESP.getFreeHeap());
    return OUT_OF_MEMORY;
  }

  WiFiClientSecure secureClient;
  HTTPClient http;

  if (_insecure) {
    secureClient.setInsecure();
    http.begin(secureClient, remoteURL);
  } else {
    http.begin(remoteURL);
  }

  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent(_userAgent);

  // Free reserve immediately before TLS handshake so mbedtls can claim it
  free(sslReserve);

  int getCode = http.GET();
  if (getCode != HTTP_CODE_OK) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] GET failed. HTTP code: %d. Aborting.\n", localPath.c_str(), getCode);
    http.end();
    if (attempt < _retryCount) continue; // retry on transient failure
    return SERVER_ERROR;
  }

  ensureDirExists(localPath);

  String tmpPath = localPath + ".tmp";
  String bakPath = localPath + ".bak";

  // Discard leftovers from an interrupted attempt. A stale .tmp must never be
  // mistaken for a completed transfer.
  if (_fs.exists(tmpPath)) _fs.remove(tmpPath);
  if (_fs.exists(bakPath)) _fs.remove(bakPath);

  // HTTPClient reports -1 when there is no Content-Length (chunked or
  // close-delimited), in which case the body framing is validated while streaming.
  int32_t expectedSize = (int32_t)http.getSize();
  bool chunkedBody = (expectedSize < 0);

  WiFiClient* stream = http.getStreamPtr();
  size_t totalBytesWritten = 0;
  bool storageError = false;

  if (verbose) Serial.printf("[ESPFileUpdater: %s] [Update] Downloading (%d bytes%s)...\n",
                              localPath.c_str(), (int)expectedSize,
                              chunkedBody ? ", length unknown" : "");

  if (!copyBodyToFile(*stream, tmpPath, chunkedBody, expectedSize,
                      totalBytesWritten, storageError, localPath, verbose)) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Incomplete download (%u bytes received). Partial file discarded.\n",
                                localPath.c_str(), (unsigned)totalBytesWritten);
    http.end();
    if (attempt < _retryCount && !storageError) continue; // retry interrupted transfers
    return storageError ? FS_ERROR : SERVER_ERROR;
  }

  if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Wrote %u bytes to file (verified)\n",
                              localPath.c_str(), (unsigned)totalBytesWritten);

  // The staged file must be exactly what was received before it is allowed to
  // replace the destination file.
  File staged = _fs.open(tmpPath, FILE_READ);
  bool stagedOpened = (bool)staged;
  size_t stagedSize = stagedOpened ? staged.size() : 0;
  if (stagedOpened) staged.close();
  if (!stagedOpened || stagedSize != totalBytesWritten) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Temp file verification failed (%u bytes on flash, %u received). Aborting.\n",
                                localPath.c_str(), (unsigned)stagedSize, (unsigned)totalBytesWritten);
    _fs.remove(tmpPath);
    http.end();
    return FS_ERROR;
  }

  http.end();

  // The .meta file is invalidated inside the swap, immediately before the old file
  // is moved aside, so it can never describe a missing or partial file.
  if (!swapInVerifiedFile(localPath, tmpPath, totalBytesWritten, verbose)) {
    return FS_ERROR;
  }

  if (verbose) Serial.printf("[ESPFileUpdater: %s] [Complete] File downloaded.\n", localPath.c_str());

  // No maxAge means no .meta file is tracked for this file (see README).
  if (maxAge.length() == 0) {
    return UPDATED;
  }

  if (newHash == "") {
    File localFile = _fs.open(localPath, FILE_READ);
    if (!localFile) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Cannot hash installed file. .meta file not written.\n", localPath.c_str());
      return UPDATED;
    }
    newHash = calculateFileHash(localFile);
    localFile.close();
  }

  newLastModified = String((uint32_t)time(nullptr));

  // The .meta file is written only now, after the destination holds a verified,
  // complete file with a confirmed size.
  if (!writeMeta(metaPath(localPath), remoteURL, newLastModified, newHash, totalBytesWritten)) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] File updated but .meta file could not be written.\n", localPath.c_str());
  }

  break; // success, exit retry loop
  } // end retry loop (download phase)

  return UPDATED;
}

/// @copydoc ESPFileUpdater::checkAndUpdate(const String&, const String&, bool)
ESPFileUpdater::UpdateStatus ESPFileUpdater::checkAndUpdate(const String& localPath, const String& remoteURL, bool verbose) {
  return checkAndUpdate(localPath, remoteURL, "", verbose);
}

/// @copydoc ESPFileUpdater::isRemoteFileNewer
ESPFileUpdater::UpdateStatus ESPFileUpdater::isRemoteFileNewer(const String& localPath, const String& remoteURL,
                                                               const String& lastModified,
                                                               int32_t remoteSize,
                                                               String& newLastModified,
                                                               String& remoteHash,
                                                               bool verbose) {
  String meta = metaPath(localPath);
  String storedLastMod = readMetaLastModified(meta);
  String storedHash = readMetaHash(meta);

  if (lastModified.length() == 0) {
    // Fallback: hash both files or check GET status
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Fallback] No Last-Modified header or server ignored request. Trying GET for file hash...\n", localPath.c_str());

    WiFiClientSecure secureClient;
    HTTPClient http;

    if (_insecure) {
      secureClient.setInsecure();
      http.begin(secureClient, remoteURL);
    } else {
      http.begin(remoteURL);
    }

    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setUserAgent(_userAgent);
    int code = http.GET();
    if (code == HTTP_CODE_NOT_FOUND) {
      http.end();
      return FILE_NOT_FOUND;
    }
    if (code == -1) {
      http.end();
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Connection failed. Library error: %s\n", localPath.c_str(), http.errorToString(code).c_str());
      // If HEAD succeeded (HTTP 200) but the GET for hashing failed, assume file is different and update
      // This handles the case where a concurrent SSL connection (e.g. audio stream) exhausts TLS resources
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Fallback] HEAD returned 200, forcing update despite hash failure.\n", localPath.c_str());
      return UPDATED;
    }
    if (code != HTTP_CODE_OK) {
      http.end();
      return SERVER_ERROR;
    }

    int32_t streamSize = (int32_t)http.getSize();
    bool chunked = (streamSize < 0);
    remoteHash = calculateStreamHash(*http.getStreamPtr(), _maxSize, chunked);
    http.end();

    File localFile = _fs.open(localPath, FILE_READ);
    if (!localFile) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] Local file could not be opened. Forcing download.\n", localPath.c_str());
      return UPDATED;
    }
    size_t localSize = localFile.size();
    String localHash = calculateFileHash(localFile);
    localFile.close();

    // Prefer the size reported by the GET, fall back to the HEAD value.
    int32_t effectiveSize = (streamSize >= 0) ? streamSize : remoteSize;

    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Local SHA256] %s (%u bytes)\n", localPath.c_str(), localHash.c_str(), (unsigned)localSize);
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Remote SHA256] %s (%d bytes)\n", localPath.c_str(), remoteHash.c_str(), (int)effectiveSize);

    if (effectiveSize >= 0 && (size_t)effectiveSize != localSize) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Check] File sizes differ. Update required.\n", localPath.c_str());
      return UPDATED;
    }

    // A hash of only the first _maxSize bytes cannot prove that two larger files are
    // identical, so never report "not modified" when the comparison window is short.
    if (remoteHash.length() == 0 || effectiveSize < 0 || (size_t)effectiveSize > _maxSize) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Check] Cannot verify with the %u byte hashing window. Update required.\n",
                                  localPath.c_str(), (unsigned)_maxSize);
      return UPDATED;
    }

    if (localHash != remoteHash) {
      return UPDATED;
    } else {
      // Update .meta file with current time, verified size and hash
      time_t now = time(nullptr);
      String nowStr = String((uint32_t)now);
      String urlStored = readMetaURL(meta);
      if (urlStored.length() == 0) urlStored = remoteURL;
      writeMeta(meta, urlStored, nowStr, localHash, localSize);
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Check] Hashes match, updated .meta file date to now.\n", localPath.c_str());
      return NOT_MODIFIED;
    }
  }

  if (lastModified != storedLastMod) {
    newLastModified = lastModified;
    return UPDATED;
  }

  return NOT_MODIFIED;
}

String ESPFileUpdater::metaPath(const String& filePath) {
  return filePath + ".meta";
}

String ESPFileUpdater::readMetaLastModified(const String& metaPath) {
  File meta = _fs.open(metaPath, FILE_READ);
  if (!meta) return "";
  meta.readStringUntil('\n'); // skip URL
  String line = meta.readStringUntil('\n');
  meta.close();
  line.trim();
  return line;
}

String ESPFileUpdater::readMetaHash(const String& metaPath) {
  File meta = _fs.open(metaPath, FILE_READ);
  if (!meta) return "";
  meta.readStringUntil('\n'); // skip URL
  meta.readStringUntil('\n'); // skip Last-Modified
  String line = meta.readStringUntil('\n');
  meta.close();
  line.trim();
  return line;
}

String ESPFileUpdater::readMetaSize(const String& metaPath) {
  File meta = _fs.open(metaPath, FILE_READ);
  if (!meta) return "";
  meta.readStringUntil('\n'); // skip URL
  meta.readStringUntil('\n'); // skip Last-Modified
  meta.readStringUntil('\n'); // skip Hash
  String line = meta.readStringUntil('\n');
  meta.close();
  line.trim();
  return line;
}

String ESPFileUpdater::readMetaURL(const String& metaPath) {
  File meta = _fs.open(metaPath, FILE_READ);
  if (!meta) return "";
  String line = meta.readStringUntil('\n');
  meta.close();
  line.trim();
  return line;
}

bool ESPFileUpdater::writeMeta(const String& metaPath, const String& url, const String& lastModified,
                               const String& sha256, size_t fileSize) {
  File meta = _fs.open(metaPath, FILE_WRITE);
  if (!meta) return false;
  meta.println(url);
  meta.println(lastModified);
  meta.println(sha256);
  meta.println((unsigned long)fileSize);
  meta.close();

  // Read the size back so a partial/failed metadata write is never reported as success.
  String written = readMetaSize(metaPath);
  return written.length() > 0 && strtoul(written.c_str(), NULL, 10) == (unsigned long)fileSize;
}

long ESPFileUpdater::parseChunkSizeLine(const String& line) {
  String s = line;
  s.trim();
  if (s.length() == 0) return -1;

  // Chunk extensions are permitted by RFC 7230 ("1a;ext=value").
  int semi = s.indexOf(';');
  if (semi >= 0) s = s.substring(0, semi);
  s.trim();
  if (s.length() == 0 || s.length() > 8) return -1;

  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    bool isHex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
    // Reject anything else (e.g. "0x1f" or decimal text) instead of misparsing it.
    if (!isHex) return -1;
  }

  long value = strtol(s.c_str(), NULL, 16);
  if (value < 0 || value > ESPFILEUPDATER_MAX_CHUNK_SIZE) return -1;
  return value;
}

bool ESPFileUpdater::copyBodyToFile(WiFiClient& stream, const String& filePath, bool chunked,
                                    int32_t contentLength, size_t& bytesWritten,
                                    bool& storageError, const String& localPath, bool verbose) {
  bytesWritten = 0;
  storageError = false;

  File file = _fs.open(filePath, FILE_WRITE);
  if (!file) {
    storageError = true;
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Cannot open temp file for writing. Aborting.\n", localPath.c_str());
    return false;
  }

  const size_t bufSize = (_bufferSize == 0) ? 1024 : _bufferSize;
  uint8_t* buf = new (std::nothrow) uint8_t[bufSize];
  if (!buf) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Failed to allocate buffer (heap: %u)\n", localPath.c_str(), (unsigned)ESP.getFreeHeap());
    file.close();
    _fs.remove(filePath);
    return false;
  }

  bool writeOk = true;
  bool terminalChunk = false;
  uint32_t lastYieldTime = millis();

  if (chunked) {
    // With no Content-Length the body is usually chunk framed, but a
    // close-delimited response looks the same to HTTPClient. Probe the first line.
    String firstLine = stream.readStringUntil('\n');
    long chunkSize = parseChunkSizeLine(firstLine);

    if (chunkSize < 0) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Info] No chunk framing detected, copying body verbatim.\n", localPath.c_str());
      size_t lineLen = firstLine.length();
      if (lineLen > 0) {
        if (file.write((const uint8_t*)firstLine.c_str(), lineLen) != lineLen) {
          storageError = true;
          writeOk = false;
        } else {
          bytesWritten += lineLen;
        }
      }
      // Only re-add the consumed delimiter when a line was actually probed, so an
      // empty body does not turn into a one byte file.
      if (writeOk && lineLen > 0) {
        if (file.write((const uint8_t*)"\n", 1) != 1) {
          storageError = true;
          writeOk = false;
        } else {
          bytesWritten += 1;
        }
      }
      chunked = false;
    } else {
      while (writeOk) {
        if (chunkSize == 0) {
          // Genuine terminal chunk: the only clean end of a chunked body.
          terminalChunk = true;
          break;
        }

        size_t remaining = (size_t)chunkSize;
        while (remaining > 0 && writeOk) {
          size_t toRead = (remaining > bufSize) ? bufSize : remaining;
          size_t got = stream.readBytes(buf, toRead);
          if (got == 0) { writeOk = false; break; } // truncated mid-chunk
          if (file.write(buf, got) != got) {
            storageError = true;
            writeOk = false;
            break;
          }
          bytesWritten += got;
          remaining -= got;
          yieldIfDue(lastYieldTime, _yieldInterval);
        }
        if (!writeOk) break;

        // The chunk payload must be terminated by CRLF (a bare LF is tolerated).
        int c1 = stream.read();
        if (c1 == '\r') {
          if (stream.read() != '\n') { writeOk = false; break; }
        } else if (c1 != '\n') {
          writeOk = false;
          break;
        }

        String sizeLine = stream.readStringUntil('\n');
        chunkSize = parseChunkSizeLine(sizeLine);
        if (chunkSize < 0) { writeOk = false; break; }
      }

      if (!writeOk && verbose) {
        Serial.printf("[ESPFileUpdater: %s] [Error] Chunked transfer framing invalid or truncated.\n", localPath.c_str());
      }
    }
  }

  if (writeOk && !chunked) {
    // Either Content-Length is known, or the body is close-delimited: copy until
    // the peer stops sending. Completeness is checked below.
    while (true) {
      size_t got = stream.readBytes(buf, bufSize);
      if (got == 0) break;
      if (file.write(buf, got) != got) {
        storageError = true;
        writeOk = false;
        break;
      }
      bytesWritten += got;
      yieldIfDue(lastYieldTime, _yieldInterval);
    }
  }

  delete[] buf;
  file.close();

  if (!writeOk) {
    _fs.remove(filePath);
    return false;
  }

  if (chunked && !terminalChunk) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Chunked body ended without a terminal chunk.\n", localPath.c_str());
    _fs.remove(filePath);
    return false;
  }

  if (!chunked && contentLength >= 0 && bytesWritten != (size_t)contentLength) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Body length mismatch: received %u of %d bytes.\n",
                                localPath.c_str(), (unsigned)bytesWritten, (int)contentLength);
    _fs.remove(filePath);
    return false;
  }

  if (!chunked && contentLength < 0 && verbose) {
    Serial.printf("[ESPFileUpdater: %s] [Warning] Response has neither Content-Length nor chunk framing; completeness cannot be verified.\n", localPath.c_str());
  }

  return true;
}

bool ESPFileUpdater::swapInVerifiedFile(const String& localPath, const String& tmpPath, size_t expectedSize, bool verbose) {
  String bakPath = localPath + ".bak";
  String meta = metaPath(localPath);

  bool hadOldFile = _fs.exists(localPath);

  // Metadata is invalidated first: from here on the .meta file can never describe a
  // file that is missing or half written.
  if (_fs.exists(meta)) _fs.remove(meta);

  if (hadOldFile) {
    if (_fs.exists(bakPath)) _fs.remove(bakPath);
    if (!_fs.rename(localPath, bakPath)) {
      if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Cannot move existing file aside. Keeping current file.\n", localPath.c_str());
      _fs.remove(tmpPath);
      return false;
    }
  }

  if (!_fs.rename(tmpPath, localPath)) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Cannot install downloaded file. Restoring previous file.\n", localPath.c_str());
    if (hadOldFile) _fs.rename(bakPath, localPath);
    _fs.remove(tmpPath);
    return false;
  }

  // Confirm the installed file is the verified one, not a partial write.
  File installed = _fs.open(localPath, FILE_READ);
  bool installedOpened = (bool)installed;
  size_t installedSize = installedOpened ? installed.size() : 0;
  if (installedOpened) installed.close();

  if (!installedOpened || installedSize != expectedSize) {
    if (verbose) Serial.printf("[ESPFileUpdater: %s] [Error] Installed file is invalid (%u of %u bytes). Restoring previous file.\n",
                                localPath.c_str(), (unsigned)installedSize, (unsigned)expectedSize);
    _fs.remove(localPath);
    if (hadOldFile) _fs.rename(bakPath, localPath);
    return false;
  }

  if (hadOldFile) _fs.remove(bakPath);
  return true;
}

String ESPFileUpdater::calculateFileHash(File& file) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);

  uint8_t buffer[512];
  int len;
  size_t total = 0;
  while (total < _maxSize) {
    size_t toRead = sizeof(buffer);
    if (total + toRead > _maxSize) toRead = _maxSize - total;
    len = file.read(buffer, toRead);
    if (len <= 0) break;
    mbedtls_sha256_update_ret(&ctx, buffer, len);
    total += len;
    yield();
  }

  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  return hashToString(hash);
}

String ESPFileUpdater::calculateStreamHash(WiFiClient& stream, size_t maxBytes, bool chunked) {
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts_ret(&ctx, 0);

  uint8_t buffer[512];
  size_t total = 0;
  int len;

  if (chunked) {
    // Probe the first line: HTTPClient reports no length for both chunked and
    // close-delimited bodies.
    String firstLine = stream.readStringUntil('\n');
    long chunkSize = parseChunkSizeLine(firstLine);

    if (chunkSize < 0) {
      // Close-delimited body: hash the probed line verbatim and continue raw.
      size_t firstLineLen = firstLine.length();
      if (firstLineLen > 0 && total < maxBytes) {
        size_t toHash = (total + firstLineLen > maxBytes) ? (maxBytes - total) : firstLineLen;
        mbedtls_sha256_update_ret(&ctx, (const uint8_t*)firstLine.c_str(), toHash);
        total += toHash;
      }
      // Only hash the consumed delimiter when a line was actually probed.
      if (firstLineLen > 0 && total < maxBytes) {
        uint8_t nl = '\n';
        mbedtls_sha256_update_ret(&ctx, &nl, 1);
        total++;
      }
      chunked = false;
    } else {
      bool framingOk = true;
      while (framingOk && total < maxBytes) {
        if (chunkSize == 0) break; // terminal chunk

        size_t remaining = (size_t)chunkSize;
        while (remaining > 0 && framingOk && total < maxBytes) {
          size_t toRead = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
          if (total + toRead > maxBytes) toRead = maxBytes - total;
          len = stream.readBytes(buffer, toRead);
          if (len == 0) { framingOk = false; break; }
          mbedtls_sha256_update_ret(&ctx, buffer, len);
          total += len;
          remaining -= len;
          yield();
        }

        // Drain the remainder of a chunk that lies beyond the hashing window.
        while (remaining > 0) {
          size_t toRead = (remaining > sizeof(buffer)) ? sizeof(buffer) : remaining;
          len = stream.readBytes(buffer, toRead);
          if (len == 0) { framingOk = false; break; }
          remaining -= len;
        }
        if (!framingOk || total >= maxBytes) break;

        // Chunk payloads are terminated by CRLF (a bare LF is tolerated).
        int c1 = stream.read();
        if (c1 == '\r') {
          if (stream.read() != '\n') { framingOk = false; break; }
        } else if (c1 != '\n') {
          framingOk = false;
          break;
        }

        String sizeLine = stream.readStringUntil('\n');
        chunkSize = parseChunkSizeLine(sizeLine);
        if (chunkSize < 0) { framingOk = false; break; }
      }

      if (!framingOk) {
        // The hash would describe framing garbage, so report it as unusable and let
        // the caller fall back to a full download.
        mbedtls_sha256_free(&ctx);
        return "";
      }

      uint8_t hash[32];
      mbedtls_sha256_finish_ret(&ctx, hash);
      mbedtls_sha256_free(&ctx);
      return hashToString(hash);
    }
  }

  // Raw payload (Content-Length known, or a close-delimited body probed above).
  while (total < maxBytes) {
    size_t toRead = sizeof(buffer);
    if (total + toRead > maxBytes) toRead = maxBytes - total;
    len = stream.readBytes(buffer, toRead);
    if (len <= 0) break;
    mbedtls_sha256_update_ret(&ctx, buffer, len);
    total += len;
    yield();
  }

  uint8_t hash[32];
  mbedtls_sha256_finish_ret(&ctx, hash);
  mbedtls_sha256_free(&ctx);

  return hashToString(hash);
}

bool ESPFileUpdater::ensureDirExists(const String& path) {
  int lastSlash = path.lastIndexOf('/');
  if (lastSlash <= 0) return true;

  String dir = path.substring(0, lastSlash);
  if (!_fs.exists(dir)) {
    return _fs.mkdir(dir);
  }
  return true;
}

time_t ESPFileUpdater::parseMaxAge(const String& maxAgeStr) {
  String s = maxAgeStr;
  s.toLowerCase();
  s.trim();
  s.replace(" ", "");
  // Find where the digits end and the unit begins
  int idx = 0;
  while (idx < s.length() && isDigit(s[idx])) idx++;
  if (idx == 0) return 0; // No number found

  int num = s.substring(0, idx).toInt();
  String unit = s.substring(idx);
  unit.replace("s", ""); // ignore plural

  if (unit == "hour" || unit == "hr" || unit == "h")
    return num * 3600;
  if (unit == "day" || unit == "d")
    return num * 86400;
  if (unit == "week" || unit == "wk" || unit == "w")
    return num * 604800; // 7 days
  if (unit == "month" || unit == "mo" || unit == "m")
    return num * 2592000; // 30 days

  return 0;
}

time_t ESPFileUpdater::parseMetaTime(const String& metaPath) {
  File meta = _fs.open(metaPath, FILE_READ);
  if (!meta) return 0;
  meta.readStringUntil('\n'); // skip URL
  String line = meta.readStringUntil('\n');
  meta.close();
  // Try to parse as epoch seconds
  line.trim();
  if (line.length() == 0) return 0;
  // If it's a number, treat as epoch
  if (line.toInt() > 100000) return line.toInt();
  // Otherwise, try to parse HTTP date
  struct tm t;
  if (strptime(line.c_str(), "%a, %d %b %Y %H:%M:%S", &t)) {
    return mktime(&t);
  }
  return 0;
}

bool ESPFileUpdater::waitForSystemReadyFS() {
  uint32_t start = millis();
  if (&_fs == nullptr) return false;
  const char* tmpname = "/.fsreadycheck135792468.tmp";
  while (millis() - start < _timeout) {
    File f = _fs.open(tmpname, FILE_WRITE);
    if (f) {
      f.print("test");
      f.close();
      _fs.remove(tmpname);
      return true;
    }
    delay(50);
    yield();
  }
  return false;
}

bool ESPFileUpdater::waitForSystemReadyTime() {
  uint32_t start = millis();
  time_t now = time(nullptr);
  while (now < 100000 && millis() - start < _timeout) {
    delay(50);
    now = time(nullptr);
    yield();
  }
  if (now < 100000) return false;
  return true;
}

bool ESPFileUpdater::waitForSystemReadyNetwork() {
  uint32_t start = millis();
  while (millis() - start < _timeout) {
    if (networkIsConnected()) return true;
    delay(500);
    yield();
  }
  return false;
}
