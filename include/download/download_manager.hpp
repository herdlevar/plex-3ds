#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <atomic>

#ifdef __3DS__
#include <3ds.h>
#endif

struct DownloadProgress {
    bool active = false;
    std::string ratingKey;
    std::string title;
    int64_t bytesDownloaded = 0;
    int64_t totalBytes = 0;
    int percent = 0;
    std::string statusText;
    bool completed = false;
    bool failed = false;
};

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    bool init();
    void exit();

    // Query downloads
    bool isDownloaded(const std::string& ratingKey) const;
    std::string getLocalFilePath(const std::string& ratingKey) const;
    std::vector<PlexMediaItem> getDownloadedItems();
    int64_t getSDFreeSpaceBytes();
    int64_t getSDTotalSpaceBytes();

    // Actions
    bool startDownload(const PlexMediaItem& item, const std::string& downloadUrl);
    void cancelDownload();
    bool deleteDownload(const std::string& ratingKey);
    bool updatePlaybackOffset(const std::string& ratingKey, int64_t offsetMs);

    // Status
    bool isDownloading() const { return m_isDownloading.load(); }
    DownloadProgress getProgress();

private:
    std::atomic<bool> m_isDownloading{false};
    std::atomic<bool> m_cancelRequested{false};

    DownloadProgress m_progress;

    PlexMediaItem m_currentItem;
    std::string m_currentUrl;

#ifdef __3DS__
    Thread m_thread = nullptr;
    static void downloadThreadEntry(void* arg);
    void downloadLoop();
#endif

    void saveMetadata(const PlexMediaItem& item, const std::string& filePath, int64_t fileSize);
    void ensureDirectories();
};
