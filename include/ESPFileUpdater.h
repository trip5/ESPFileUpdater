// ESPFileUpdater 1.5.0 -- Created by Trip5 : https://github.com/trip5/ESPFileUpdater

#pragma once

#include <Arduino.h>
#include <HTTPClient.h>
#include <FS.h>
#include <mbedtls/sha256.h>
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include <WiFi.h>
#ifdef ETH
  #include <ETH.h>
#endif

/// @brief Class for updating files on ESP devices from a remote HTTP source.
class ESPFileUpdater {
public:

// new setters to override defaults at runtime
  void setMaxSize(size_t bytes)       { _maxSize = bytes; }
  void setTimeout(uint32_t ms)        { _timeout = ms; }
  void setUserAgent(const String& ua) { _userAgent = ua; }
  void setInsecure(bool insecure)     { _insecure = insecure; }
  void setBuffer(size_t bytes)        { _bufferSize = bytes; }
  void setYieldInterval(uint32_t ms)  { _yieldInterval = ms; }
  void setRetryCount(uint8_t count)   { _retryCount = count; }
  void setRetryDelay(uint32_t ms)     { _retryDelay = ms; }

  /// @brief Status codes for update attempts.
  enum UpdateStatus {
    UPDATED,              ///< File was updated.
    MAX_AGE_NOT_REACHED,  ///< Max age not reached, update skipped.
    NOT_MODIFIED,         ///< File is already up-to-date.
    SERVER_ERROR,         ///< Server error or unreachable.
    FILE_NOT_FOUND,       ///< Remote file not found (404).
    FS_ERROR,             ///< Filesystem error.
    TIME_ERROR,           ///< System time not set.
    NETWORK_ERROR,        ///< Network connection not ready.
    CONNECTION_FAILED,    ///< Library connection error
    OUT_OF_MEMORY         ///< Insufficient heap memory
  };

  /// @brief Construct a new ESPFileUpdater object.
  /// @param fs Reference to the filesystem (SPIFFS, LittleFS, etc.)
  ESPFileUpdater(fs::FS& fs);

  /// @brief Check and update a file from a remote URL, with optional max-age and verbose logging.
  /// @param localPath Path to the local file.
  /// @param remoteURL URL of the remote file.
  /// @param maxAge Optional: max age string (e.g., "12h", "5 days", "2weeks", "1 month" - may be specified as abbreviations: h, hr, d, wk, w, mo, m)
  /// @param verbose Optional: enable verbose logging.
  /// @return UpdateStatus indicating the result.
  UpdateStatus checkAndUpdate(const String& localPath, const String& remoteURL, const String& maxAge = "", bool verbose = false);

  /// @brief Check and update a file from a remote URL, with verbose logging.
  /// @param localPath Path to the local file.
  /// @param remoteURL URL of the remote file.
  /// @param verbose Enable verbose logging.
  /// @return UpdateStatus indicating the result.
  UpdateStatus checkAndUpdate(const String& localPath, const String& remoteURL, bool verbose);

private:
  fs::FS& _fs;
  size_t   _maxSize       = 102400;    // 100 KB max stream size for hashing
  uint32_t _timeout       = 15000;     // 15000ms / 15s for timeout (for each check)
  bool     _insecure      = false;     // insecure mode enabled
  size_t   _bufferSize    = 2048;      // buffer size for reading/writing during downloads
  uint32_t _yieldInterval = 20;        // yield every X milliseconds (time-based for consistent responsiveness)
  uint8_t  _retryCount    = 2;         // number of retries on transient failures
  uint32_t _retryDelay    = 2000;      // delay between retries in ms
                                       // the browser / agent to report to servers
  String   _userAgent = "ESPFileUpdater/1.5.0 (https://github.com/trip5/ESPFileUpdater)";


  /// @brief Wait until the filesystem is ready (SPIFFS mounted).
  /// @return true if ready, false if timeout.
  bool waitForSystemReadyFS();
  /// @brief Wait until the system time is set.
  /// @return true if ready, false if timeout.
  bool waitForSystemReadyTime();
  
  static bool networkIsConnected() {
    if (WiFi.isConnected()) return true;
  #ifdef ETH
    if (ETH.linkUp()) return true;
  #endif
    return false;
  }

  /// @brief Wait until Network is connected (WL_CONNECTED).
  /// @return true if ready, false if timeout.
  bool waitForSystemReadyNetwork();

  /// @brief Get the path to the .meta file for a given file.
  /// @param filePath Path to the local file.
  /// @return Path to the .meta file.
  String metaPath(const String& filePath);

  /// @brief Read the last-modified value from the .meta file.
  /// @param metaPath Path to the .meta file.
  /// @return Last-modified string.
  String readMetaLastModified(const String& metaPath);

  /// @brief Read the hash value from the .meta file.
  /// @param metaPath Path to the .meta file.
  /// @return Hash string.
  String readMetaHash(const String& metaPath);

  /// @brief Read the recorded file size from the .meta file.
  /// @param metaPath Path to the .meta file.
  /// @return Size as a string, empty when the file predates size tracking.
  String readMetaSize(const String& metaPath);

  /// @brief Read the original URL from the .meta file.
  /// @param metaPath Path to the .meta file.
  /// @return URL string.
  String readMetaURL(const String& metaPath);

  /// @brief Write the .meta file with URL, last-modified, hash and file size.
  /// @param metaPath Path to the .meta file.
  /// @param url Source URL.
  /// @param lastModified Last-modified string (epoch seconds).
  /// @param sha256 SHA256 hash string.
  /// @param fileSize Verified size of the file on disk, in bytes.
  /// @return true if successful, false otherwise.
  bool writeMeta(const String& metaPath, const String& url, const String& lastModified,
                 const String& sha256, size_t fileSize);

  /// @brief Copy an HTTP response body into a file, verifying that it is complete.
  ///        Short writes, truncated bodies, missing terminal chunks and framing
  ///        errors are all treated as failures.
  /// @param stream Response byte stream.
  /// @param filePath Destination file (created/truncated).
  /// @param chunked true when the body may use chunked transfer encoding.
  /// @param contentLength Expected body length from Content-Length, or -1 when unknown.
  /// @param bytesWritten Output: payload bytes written to the file.
  /// @param storageError Output: true when the failure was a filesystem/write error.
  /// @param localPath Path used only for log prefixes.
  /// @param verbose Enable verbose logging.
  /// @return true only when every byte was received and written.
  bool copyBodyToFile(WiFiClient& stream, const String& filePath, bool chunked,
                      int32_t contentLength, size_t& bytesWritten, bool& storageError,
                      const String& localPath, bool verbose);

  /// @brief Install a verified temp file next to a destination file.
  ///        The .meta file is invalidated before the old file is moved aside, and the
  ///        previous file is restored if the install or its verification fails.
  /// @param localPath Destination path.
  /// @param tmpPath Verified temp file to install.
  /// @param expectedSize Size the installed file must have.
  /// @param verbose Enable verbose logging.
  /// @return true if the destination now holds exactly expectedSize bytes.
  bool swapInVerifiedFile(const String& localPath, const String& tmpPath, size_t expectedSize, bool verbose);

  /// @brief Parse a chunk-size line from a chunked transfer.
  /// @param line Chunk-size line, possibly with a chunk extension.
  /// @return Chunk size in bytes (0 for the terminal chunk), or -1 if not valid framing.
  static long parseChunkSizeLine(const String& line);

  /// @brief Calculate the SHA256 hash of a file.
  /// @param file Open file handle.
  /// @return SHA256 hash string.
  String calculateFileHash(File& file);

  /// @brief Calculate the SHA256 hash of a stream.
  /// @param stream WiFiClient stream.
  /// @param maxBytes Maximum bytes to read.
  /// @param chunked true when the body may use chunked transfer encoding.
  /// @return SHA256 hash string, or empty when the body could not be read reliably.
  String calculateStreamHash(WiFiClient& stream, size_t maxBytes, bool chunked);

  /// @brief Ensure the directory for a given path exists.
  /// @param path File path.
  /// @return true if directory exists or was created, false otherwise.
  bool ensureDirExists(const String& path);

  /// @brief Check if a file exists.
  /// @param path File path.
  /// @return true if file exists, false otherwise.
  bool fileExists(const String& path);

  /// @brief Parse a max-age string (e.g., "7d") to seconds.
  /// @param maxAgeStr Max-age string.
  /// @return Number of seconds.
  time_t parseMaxAge(const String& maxAgeStr);

  /// @brief Parse the last-modified time from the .meta file.
  /// @param metaPath Path to the .meta file.
  /// @return Time as epoch seconds.
  time_t parseMetaTime(const String& metaPath);

  /// @brief Determine if the remote file is newer than the local file.
  /// @param localPath Path to the local file.
  /// @param url Remote file URL.
  /// @param lastModified Last-modified string from server.
  /// @param remoteSize Size reported by the server, or -1 when unknown.
  /// @param newLastModified Output: new last-modified string.
  /// @param remoteHash Output: remote file hash.
  /// @param verbose Enable verbose logging.
  /// @return UpdateStatus indicating if update is needed.
  UpdateStatus isRemoteFileNewer(const String& localPath, const String& url,
                                 const String& lastModified,
                                 int32_t remoteSize,
                                 String& newLastModified,
                                 String& remoteHash,
                                 bool verbose);
};
