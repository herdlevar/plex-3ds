#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <mutex>

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
    int queueCount = 0;
    int queueIndex = 0;
    int completedCount = 0;
};

struct QueuedDownload {
    PlexMediaItem item;
    std::string url;
};

class DownloadManager {
public:
    DownloadManager();
    ~DownloadManager();

    bool init();
    void exit();

    // Query downloads
    bool isDownloaded(const std::string& ratingKey) const;
    bool isQueued(const std::string& ratingKey) const;
    int getQueuePosition(const std::string& ratingKey) const;
    std::string getLocalFilePath(const std::string& ratingKey) const;
    std::vector<PlexMediaItem> getDownloadedItems();
    int64_t getSDFreeSpaceBytes();
    int64_t getSDTotalSpaceBytes();

    // Actions
    bool startDownload(const PlexMediaItem& item, const std::string& downloadUrl);
    int queueDownloads(const std::vector<std::pair<PlexMediaItem, std::string>>& items);
    void cancelDownload();
    bool cancelQueuedItem(const std::string& ratingKey);
    bool deleteDownload(const std::string& ratingKey);
    bool updatePlaybackOffset(const std::string& ratingKey, int64_t offsetMs);

    // Path generation
    static std::string sanitizePathComponent(const std::string& name);
    static std::string buildLocalMediaPath(const PlexMediaItem& item);

    // Status
    bool isDownloading() const { return m_isDownloading.load(); }
    DownloadProgress getProgress();
    int getQueueSize();
    void setClientIdentifier(const std::string& clientId) { m_clientIdentifier = clientId; }

private:
    std::string m_clientIdentifier;
    std::atomic<bool> m_isDownloading{false};
    std::atomic<bool> m_cancelRequested{false};
    std::atomic<int> m_completedCount{0};

    DownloadProgress m_progress;
    std::mutex m_progressMutex;

    PlexMediaItem m_currentItem;
    std::string m_currentUrl;

    std::vector<QueuedDownload> m_queue;
    std::mutex m_queueMutex;
    int m_totalQueueCount = 0;
    int m_currentQueueIndex = 0;

#ifdef __3DS__
    Thread m_thread = nullptr;
    static void downloadThreadEntry(void* arg);
    void downloadLoop();
#endif

    void saveMetadata(const PlexMediaItem& item, const std::string& filePath, int64_t fileSize);
    void ensureDirectories();
};
