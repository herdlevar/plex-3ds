#include "download/download_manager.hpp"
#include "cJSON.h"

#ifdef __3DS__
#include <3ds.h>
#include <sys/statvfs.h>
#endif

#include <curl/curl.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <algorithm>

static const std::string BASE_DOWNLOAD_DIR   = "sdmc:/3ds/plex-3ds/downloads";
static const std::string MOVIES_DOWNLOAD_DIR = "sdmc:/3ds/plex-3ds/downloads/Movies";
static const std::string TV_DOWNLOAD_DIR     = "sdmc:/3ds/plex-3ds/downloads/TV Shows";
static const std::string MUSIC_DOWNLOAD_DIR  = "sdmc:/3ds/plex-3ds/downloads/Music";
static const std::string META_DOWNLOAD_DIR   = "sdmc:/3ds/plex-3ds/downloads/meta";
static const std::string LEGACY_VIDEO_DIR    = "sdmc:/3ds/plex-3ds/downloads/videos";
static const std::string LEGACY_MUSIC_DIR    = "sdmc:/3ds/plex-3ds/downloads/music";

static std::string sanitizeKey(const std::string& key) {
    std::string safe;
    for (char c : key) {
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            safe += c;
        }
    }
    return safe.empty() ? "item" : safe;
}

std::string DownloadManager::sanitizePathComponent(const std::string& name) {
    if (name.empty()) return "Unknown";
    std::string safe;
    safe.reserve(name.size());
    for (size_t i = 0; i < name.size(); i++) {
        char c = name[i];
        if (c == ':' && (i + 1 < name.size() && name[i + 1] == ' ')) {
            safe += " -";
        } else if (c == ':' || c == '/' || c == '\\' || c == '*' || c == '?' ||
                   c == '"' || c == '<' || c == '>' || c == '|' || (unsigned char)c < 32) {
            safe += '_';
        } else {
            safe += c;
        }
    }
    // Trim leading whitespace, dots, and underscores
    size_t start = 0;
    while (start < safe.size() && (safe[start] == ' ' || safe[start] == '.' || safe[start] == '_')) {
        start++;
    }
    // Trim trailing whitespace, dots, and underscores
    size_t end = safe.size();
    while (end > start && (safe[end - 1] == ' ' || safe[end - 1] == '.' || safe[end - 1] == '_')) {
        end--;
    }
    if (start >= end) return "Unknown";
    return safe.substr(start, end - start);
}

static void createDirectories(const std::string& dirPath) {
#ifdef __3DS__
    size_t pos = 0;
    if (dirPath.rfind("sdmc:/", 0) == 0) {
        pos = 6;
    }
    while ((pos = dirPath.find('/', pos)) != std::string::npos) {
        std::string sub = dirPath.substr(0, pos);
        if (!sub.empty() && sub != "sdmc:") {
            mkdir(sub.c_str(), 0777);
        }
        pos++;
    }
    mkdir(dirPath.c_str(), 0777);
#endif
}

std::string DownloadManager::buildLocalMediaPath(const PlexMediaItem& item) {
    std::string safeTitle = sanitizePathComponent(item.title);
    if (safeTitle.empty() || safeTitle == "Unknown") {
        safeTitle = sanitizeKey(item.ratingKey);
    }

    if (item.type == MediaType::TRACK) {
        std::string artist = !item.grandparentTitle.empty() ? item.grandparentTitle : "Unknown Artist";
        std::string album = !item.parentTitle.empty() ? item.parentTitle : "Unknown Album";
        std::string safeArtist = sanitizePathComponent(artist);
        std::string safeAlbum = sanitizePathComponent(album);
        std::string dir = MUSIC_DOWNLOAD_DIR + "/" + safeArtist + "/" + safeAlbum;
        createDirectories(dir);

        char numBuf[32];
        if (item.index > 0) {
            snprintf(numBuf, sizeof(numBuf), "%02d - ", item.index);
            return dir + "/" + numBuf + safeTitle + ".mp3";
        } else {
            return dir + "/" + safeTitle + ".mp3";
        }
    } else if (item.type == MediaType::EPISODE || (!item.grandparentTitle.empty() && item.type != MediaType::MOVIE)) {
        std::string show = !item.grandparentTitle.empty() ? item.grandparentTitle : (!item.parentTitle.empty() ? item.parentTitle : "Unknown Show");
        std::string season = !item.parentTitle.empty() ? item.parentTitle : "Season 01";
        std::string safeShow = sanitizePathComponent(show);
        std::string safeSeason = sanitizePathComponent(season);
        std::string dir = TV_DOWNLOAD_DIR + "/" + safeShow + "/" + safeSeason;
        createDirectories(dir);

        char numBuf[32];
        if (item.index > 0) {
            snprintf(numBuf, sizeof(numBuf), "%02d - ", item.index);
            return dir + "/" + numBuf + safeTitle + ".mkv";
        } else {
            return dir + "/" + safeTitle + ".mkv";
        }
    } else {
        std::string folderName = safeTitle;
        if (item.year > 0) {
            folderName += " (" + std::to_string(item.year) + ")";
        }
        std::string dir = MOVIES_DOWNLOAD_DIR + "/" + folderName;
        createDirectories(dir);

        std::string fileName = safeTitle;
        if (item.year > 0) {
            fileName += " (" + std::to_string(item.year) + ")";
        }
        return dir + "/" + fileName + ".mkv";
    }
}

DownloadManager::DownloadManager() {}

DownloadManager::~DownloadManager() {
    exit();
}

void DownloadManager::ensureDirectories() {
#ifdef __3DS__
    mkdir("sdmc:/3ds", 0777);
    mkdir("sdmc:/3ds/plex-3ds", 0777);
    mkdir(BASE_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(MOVIES_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(TV_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(MUSIC_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(META_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(LEGACY_VIDEO_DIR.c_str(), 0777);
    mkdir(LEGACY_MUSIC_DIR.c_str(), 0777);
#endif
}

bool DownloadManager::init() {
    ensureDirectories();
    return true;
}

void DownloadManager::exit() {
    cancelDownload();
}

bool DownloadManager::isDownloaded(const std::string& ratingKey) const {
    if (ratingKey.empty()) return false;
    std::string safeKey = sanitizeKey(ratingKey);
    std::string metaPath = META_DOWNLOAD_DIR + "/" + safeKey + ".json";
    FILE* f = fopen(metaPath.c_str(), "rb");
    if (!f) return false;
    fclose(f);

    std::string lp = getLocalFilePath(ratingKey);
    if (lp.empty()) return false;
    struct stat st;
    if (stat(lp.c_str(), &st) != 0 || st.st_size < 4096) {
        return false;
    }
    return true;
}

std::string DownloadManager::getLocalFilePath(const std::string& ratingKey) const {
    if (ratingKey.empty()) return "";
    std::string safeKey = sanitizeKey(ratingKey);
    std::string metaPath = META_DOWNLOAD_DIR + "/" + safeKey + ".json";
    FILE* f = fopen(metaPath.c_str(), "rb");
    if (!f) return "";

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);

    std::string path = "";
    cJSON* root = cJSON_Parse(content.c_str());
    if (root) {
        cJSON* p = cJSON_GetObjectItem(root, "localPath");
        if (p && p->valuestring) path = p->valuestring;
        cJSON_Delete(root);
    }
    return path;
}

std::vector<PlexMediaItem> DownloadManager::getDownloadedItems() {
    std::vector<PlexMediaItem> items;
    DIR* dir = opendir(META_DOWNLOAD_DIR.c_str());
    if (!dir) return items;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        std::string fname = entry->d_name;
        if (fname.length() > 5 && fname.rfind(".json") == fname.length() - 5) {
            std::string metaPath = META_DOWNLOAD_DIR + "/" + fname;
            FILE* f = fopen(metaPath.c_str(), "rb");
            if (!f) continue;

            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);

            std::string content(sz, '\0');
            fread(&content[0], 1, sz, f);
            fclose(f);

            cJSON* root = cJSON_Parse(content.c_str());
            if (root) {
                cJSON* rk = cJSON_GetObjectItem(root, "ratingKey");
                cJSON* title = cJSON_GetObjectItem(root, "title");
                cJSON* parentTitle = cJSON_GetObjectItem(root, "parentTitle");
                cJSON* gpTitle = cJSON_GetObjectItem(root, "grandparentTitle");
                cJSON* summary = cJSON_GetObjectItem(root, "summary");
                cJSON* type = cJSON_GetObjectItem(root, "type");
                cJSON* localPath = cJSON_GetObjectItem(root, "localPath");
                cJSON* fileSize = cJSON_GetObjectItem(root, "fileSize");
                cJSON* durationMs = cJSON_GetObjectItem(root, "durationMs");
                cJSON* year = cJSON_GetObjectItem(root, "year");
                cJSON* index = cJSON_GetObjectItem(root, "index");

                auto isFileValid = [](const std::string& path) -> bool {
                    if (path.empty()) return false;
                    struct stat st;
                    return (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 4096);
                };

                std::string lp = (localPath && localPath->valuestring) ? localPath->valuestring : "";
                if (lp.empty() || !isFileValid(lp)) {
                    std::string keyBase = fname.substr(0, fname.length() - 5);
                    std::string cand1 = MUSIC_DOWNLOAD_DIR + "/" + keyBase + ".mp3";
                    std::string cand2 = LEGACY_VIDEO_DIR + "/" + keyBase + ".mkv";
                    std::string cand3 = LEGACY_MUSIC_DIR + "/" + keyBase + ".mp3";
                    std::string cand4 = BASE_DOWNLOAD_DIR + "/" + keyBase + ".mp3";
                    std::string cand5 = BASE_DOWNLOAD_DIR + "/" + keyBase + ".mkv";
                    if (isFileValid(cand1)) lp = cand1;
                    else if (isFileValid(cand2)) lp = cand2;
                    else if (isFileValid(cand3)) lp = cand3;
                    else if (isFileValid(cand4)) lp = cand4;
                    else if (isFileValid(cand5)) lp = cand5;
                }

                if (isFileValid(lp)) {

                    PlexMediaItem it;
                    if (rk && rk->valuestring) it.ratingKey = rk->valuestring;
                    if (title && title->valuestring) it.title = title->valuestring;
                    if (parentTitle && parentTitle->valuestring) it.parentTitle = parentTitle->valuestring;
                    if (gpTitle && gpTitle->valuestring) it.grandparentTitle = gpTitle->valuestring;
                    if (summary && summary->valuestring) it.summary = summary->valuestring;
                    if (fileSize && cJSON_IsNumber(fileSize)) it.localFileSize = (int64_t)fileSize->valuedouble;
                    if (durationMs && cJSON_IsNumber(durationMs)) it.durationMs = (int64_t)durationMs->valuedouble;
                    cJSON* viewOffsetMs = cJSON_GetObjectItem(root, "viewOffsetMs");
                    if (viewOffsetMs && cJSON_IsNumber(viewOffsetMs)) it.viewOffsetMs = (int64_t)viewOffsetMs->valuedouble;
                    if (year && cJSON_IsNumber(year)) it.year = year->valueint;
                    if (index && cJSON_IsNumber(index)) it.index = index->valueint;

                    std::string tStr = (type && type->valuestring) ? type->valuestring : "";
                    if (tStr == "track") it.type = MediaType::TRACK;
                    else if (tStr == "episode") it.type = MediaType::EPISODE;
                    else if (tStr == "movie") {
                        if (!it.grandparentTitle.empty() || it.parentTitle.find("Season") != std::string::npos) {
                            it.type = MediaType::EPISODE;
                        } else {
                            it.type = MediaType::MOVIE;
                        }
                    } else if (tStr == "show") it.type = MediaType::SHOW;
                    else if (tStr == "season") it.type = MediaType::SEASON;
                    else if (tStr == "artist") it.type = MediaType::ARTIST;
                    else if (tStr == "album") it.type = MediaType::ALBUM;
                    else {
                        if (!it.grandparentTitle.empty() || it.parentTitle.find("Season") != std::string::npos) {
                            it.type = MediaType::EPISODE;
                        } else if (lp.rfind(".mp3") == lp.length() - 4) {
                            it.type = MediaType::TRACK;
                        } else {
                            it.type = MediaType::MOVIE;
                        }
                    }

                    it.isOffline = true;
                    it.localFilePath = lp;
                    it.partKey = lp;
                    it.key = lp;

                    items.push_back(it);
                }
                cJSON_Delete(root);
            }
        }
    }
    closedir(dir);

    // Recursively scan directories for any media files missing meta json files
    auto scanDirRecursive = [&](auto& self, const std::string& dirPath, int depth, MediaType defaultType, const std::string& parentFolder, const std::string& gpFolder) -> void {
        if (depth <= 0) return;
        DIR* d = opendir(dirPath.c_str());
        if (!d) return;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string name = e->d_name;
            if (name == "." || name == "..") continue;
            std::string fullPath = dirPath + "/" + name;
            struct stat st;
            if (stat(fullPath.c_str(), &st) != 0) continue;

            if (S_ISDIR(st.st_mode)) {
                self(self, fullPath, depth - 1, defaultType, name, parentFolder);
            } else if (S_ISREG(st.st_mode) && st.st_size > 4096) {
                bool isMkv = (name.length() > 4 && name.rfind(".mkv") == name.length() - 4);
                bool isMp4 = (name.length() > 4 && name.rfind(".mp4") == name.length() - 4);
                bool isMp3 = (name.length() > 4 && name.rfind(".mp3") == name.length() - 4);
                if (!isMkv && !isMp4 && !isMp3) continue;

                bool alreadyIn = false;
                for (const auto& existing : items) {
                    if (existing.localFilePath == fullPath) {
                        alreadyIn = true;
                        break;
                    }
                }
                if (alreadyIn) continue;

                std::string base = name.substr(0, name.rfind('.'));
                if (base.rfind(".temp_", 0) == 0) base = base.substr(6);
                if (base.rfind("temp_", 0) == 0) base = base.substr(5);

                PlexMediaItem it;
                it.ratingKey = base;
                it.title = base;
                it.isOffline = true;
                it.localFilePath = fullPath;
                it.partKey = fullPath;
                it.key = fullPath;
                it.localFileSize = st.st_size;

                if (isMp3) {
                    it.type = MediaType::TRACK;
                    if (!parentFolder.empty()) it.parentTitle = parentFolder;
                    if (!gpFolder.empty()) it.grandparentTitle = gpFolder;
                } else if (defaultType == MediaType::EPISODE || !gpFolder.empty()) {
                    it.type = MediaType::EPISODE;
                    if (!parentFolder.empty()) it.parentTitle = parentFolder;
                    if (!gpFolder.empty()) it.grandparentTitle = gpFolder;
                } else {
                    it.type = MediaType::MOVIE;
                }
                items.push_back(it);
            }
        }
        closedir(d);
    };

    scanDirRecursive(scanDirRecursive, MOVIES_DOWNLOAD_DIR, 3, MediaType::MOVIE, "", "");
    scanDirRecursive(scanDirRecursive, TV_DOWNLOAD_DIR, 4, MediaType::EPISODE, "", "");
    scanDirRecursive(scanDirRecursive, MUSIC_DOWNLOAD_DIR, 4, MediaType::TRACK, "", "");
    scanDirRecursive(scanDirRecursive, LEGACY_VIDEO_DIR, 2, MediaType::MOVIE, "", "");
    scanDirRecursive(scanDirRecursive, LEGACY_MUSIC_DIR, 2, MediaType::TRACK, "", "");
    scanDirRecursive(scanDirRecursive, BASE_DOWNLOAD_DIR, 1, MediaType::TRACK, "", "");
    return items;
}

int64_t DownloadManager::getSDFreeSpaceBytes() {
#ifdef __3DS__
    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        return (int64_t)vfs.f_bavail * (int64_t)vfs.f_bsize;
    }
#endif
    return 0;
}

int64_t DownloadManager::getSDTotalSpaceBytes() {
#ifdef __3DS__
    struct statvfs vfs;
    if (statvfs("sdmc:/", &vfs) == 0) {
        return (int64_t)vfs.f_blocks * (int64_t)vfs.f_bsize;
    }
#endif
    return 0;
}

bool DownloadManager::deleteDownload(const std::string& ratingKey) {
    if (ratingKey.empty()) return false;
    std::string safeKey = sanitizeKey(ratingKey);
    std::string metaPath = META_DOWNLOAD_DIR + "/" + safeKey + ".json";
    std::string localPath = getLocalFilePath(ratingKey);

    if (!localPath.empty()) {
        remove(localPath.c_str());
        // Clean up empty parent folder (e.g. Season or Album) and grandparent folder (e.g. Show or Artist)
        size_t lastSlash = localPath.rfind('/');
        if (lastSlash != std::string::npos) {
            std::string parentDir = localPath.substr(0, lastSlash);
            rmdir(parentDir.c_str());
            size_t prevSlash = parentDir.rfind('/');
            if (prevSlash != std::string::npos) {
                std::string gpDir = parentDir.substr(0, prevSlash);
                rmdir(gpDir.c_str());
            }
        }
    }
    remove(metaPath.c_str());
    return true;
}

bool DownloadManager::updatePlaybackOffset(const std::string& ratingKey, int64_t offsetMs) {
    if (ratingKey.empty()) return false;
    std::string safeKey = sanitizeKey(ratingKey);
    std::string metaPath = META_DOWNLOAD_DIR + "/" + safeKey + ".json";
    FILE* f = fopen(metaPath.c_str(), "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return false;

    cJSON* existing = cJSON_GetObjectItem(root, "viewOffsetMs");
    if (existing) {
        cJSON_ReplaceItemInObject(root, "viewOffsetMs", cJSON_CreateNumber((double)offsetMs));
    } else {
        cJSON_AddNumberToObject(root, "viewOffsetMs", (double)offsetMs);
    }

    char* jsonStr = cJSON_Print(root);
    if (jsonStr) {
        f = fopen(metaPath.c_str(), "wb");
        if (f) {
            fputs(jsonStr, f);
            fclose(f);
        }
        free(jsonStr);
    }
    cJSON_Delete(root);
    return true;
}

void DownloadManager::saveMetadata(const PlexMediaItem& item, const std::string& filePath, int64_t fileSize) {
    ensureDirectories();
    std::string safeKey = sanitizeKey(item.ratingKey);
    std::string metaPath = META_DOWNLOAD_DIR + "/" + safeKey + ".json";

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "ratingKey", item.ratingKey.c_str());
    cJSON_AddStringToObject(root, "title", item.title.c_str());
    cJSON_AddStringToObject(root, "parentTitle", item.parentTitle.c_str());
    cJSON_AddStringToObject(root, "grandparentTitle", item.grandparentTitle.c_str());
    cJSON_AddStringToObject(root, "summary", item.summary.c_str());

    const char* tStr = "movie";
    if (item.type == MediaType::TRACK) tStr = "track";
    else if (item.type == MediaType::EPISODE) tStr = "episode";
    else if (item.type == MediaType::SHOW) tStr = "show";
    else if (item.type == MediaType::SEASON) tStr = "season";
    else if (item.type == MediaType::ARTIST) tStr = "artist";
    else if (item.type == MediaType::ALBUM) tStr = "album";
    cJSON_AddStringToObject(root, "type", tStr);

    cJSON_AddStringToObject(root, "localPath", filePath.c_str());
    cJSON_AddNumberToObject(root, "fileSize", (double)fileSize);
    cJSON_AddNumberToObject(root, "durationMs", (double)item.durationMs);
    cJSON_AddNumberToObject(root, "viewOffsetMs", (double)item.viewOffsetMs);
    cJSON_AddNumberToObject(root, "year", item.year);
    cJSON_AddNumberToObject(root, "index", item.index);

    char* jsonStr = cJSON_Print(root);
    if (jsonStr) {
        FILE* f = fopen(metaPath.c_str(), "wb");
        if (f) {
            fputs(jsonStr, f);
            fclose(f);
        }
        free(jsonStr);
    }
    cJSON_Delete(root);
}

#ifdef __3DS__
void DownloadManager::downloadThreadEntry(void* arg) {
    DownloadManager* self = static_cast<DownloadManager*>(arg);
    if (self) self->downloadLoop();
}

void DownloadManager::downloadLoop() {
    ensureDirectories();

    while (!m_cancelRequested.load() && !g_appExiting.load()) {
        QueuedDownload current;
        int qIdx = 0;
        int qTot = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queue.empty()) {
                m_isDownloading = false;
                m_totalQueueCount = 0;
                m_currentQueueIndex = 0;
                break;
            }
            current = m_queue.front();
            m_queue.erase(m_queue.begin());
            m_currentQueueIndex++;
            qIdx = m_currentQueueIndex;
            qTot = m_totalQueueCount;
            m_currentItem = current.item;
            m_currentUrl = current.url;
        }

        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.active = true;
            m_progress.ratingKey = current.item.ratingKey;
            m_progress.title = current.item.title;
            m_progress.bytesDownloaded = 0;
            m_progress.totalBytes = 0;
            m_progress.percent = 0;
            m_progress.queueCount = qTot;
            m_progress.queueIndex = qIdx;
            m_progress.statusText = "Connecting (" + std::to_string(qIdx) + "/" + std::to_string(qTot) + ")...";
            m_progress.completed = false;
            m_progress.failed = false;
        }

        std::string finalPath = buildLocalMediaPath(current.item);

        FILE* outFile = fopen(finalPath.c_str(), "wb");
        if (!outFile) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Cannot create file on SD";
            m_progress.failed = true;
            continue;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            fclose(outFile);
            remove(finalPath.c_str());
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Failed to initialize curl";
            m_progress.failed = true;
            break;
        }

        struct DownloadContext {
            DownloadManager* mgr;
            FILE* fp;
        } ctx = { this, outFile };

        auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            DownloadContext* dc = (DownloadContext*)userdata;
            if (dc->mgr->m_cancelRequested.load() || g_appExiting.load()) return 0;
            while (g_isSuspended.load() && !dc->mgr->m_cancelRequested.load() && !g_appExiting.load()) {
                svcSleepThread(50000000); // 50ms pause during Home Menu
            }
            if (dc->mgr->m_cancelRequested.load() || g_appExiting.load()) return 0;
            return fwrite(ptr, 1, size * nmemb, dc->fp);
        };

        auto xferInfoCb = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
            (void)ultotal; (void)ulnow;
            DownloadManager* mgr = (DownloadManager*)clientp;
            if (mgr->m_cancelRequested.load() || g_appExiting.load()) return 1;

            while (g_isSuspended.load() && !mgr->m_cancelRequested.load() && !g_appExiting.load()) {
                svcSleepThread(50000000); // 50ms pause during Home Menu
            }
            if (mgr->m_cancelRequested.load() || g_appExiting.load()) return 1;

            std::lock_guard<std::mutex> lock(mgr->m_progressMutex);
            mgr->m_progress.bytesDownloaded = (int64_t)dlnow;
            mgr->m_progress.totalBytes = (int64_t)dltotal;

            std::string qInfo = "";
            if (mgr->m_progress.queueCount > 1) {
                qInfo = "(" + std::to_string(mgr->m_progress.queueIndex) + "/" + std::to_string(mgr->m_progress.queueCount) + ") ";
            }

            if (dltotal > 0) {
                mgr->m_progress.percent = std::clamp((int)((dlnow * 100) / dltotal), 0, 100);
                double mbNow = (double)dlnow / (1024.0 * 1024.0);
                double mbTot = (double)dltotal / (1024.0 * 1024.0);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.1f / %.1f MB (%d%%)", mbNow, mbTot, mgr->m_progress.percent);
                mgr->m_progress.statusText = qInfo + buf;
            } else {
                // If Content-Length is missing (e.g. chunked streaming transcode), estimate from duration
                int64_t estTotal = 0;
                if (mgr->m_currentItem.durationMs > 0) {
                    if (mgr->m_currentItem.type == MediaType::TRACK) {
                        estTotal = (mgr->m_currentItem.durationMs / 1000) * 16000; // ~128kbps MP3
                    } else {
                        estTotal = (mgr->m_currentItem.durationMs / 1000) * 141000; // ~1128kbps Video
                    }
                }
                if (estTotal <= 0) {
                    // Safe fallbacks for missing duration metadata
                    if (mgr->m_currentItem.type == MediaType::TRACK) {
                        estTotal = 4 * 1024 * 1024; // ~4 MB for typical song
                    } else {
                        estTotal = 150 * 1024 * 1024; // ~150 MB for typical episode
                    }
                }
                if (estTotal > 0 && dlnow > 0) {
                    mgr->m_progress.percent = std::clamp((int)((dlnow * 100) / estTotal), 0, 99);
                } else {
                    mgr->m_progress.percent = 0;
                }
                double mbNow = (double)dlnow / (1024.0 * 1024.0);
                char buf[64];
                if (mbNow >= 1.0) {
                    snprintf(buf, sizeof(buf), "%.1f MB (%d%%)", mbNow, mgr->m_progress.percent);
                } else {
                    snprintf(buf, sizeof(buf), "%d KB (%d%%)", (int)(dlnow / 1024), mgr->m_progress.percent);
                }
                mgr->m_progress.statusText = qInfo + buf;
            }
            return 0;
        };

        curl_easy_setopt(curl, CURLOPT_URL, current.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)+writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, (curl_xferinfo_callback)+xferInfoCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 32768L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

        static const char* CA_BUNDLE_PATH = "sdmc:/3ds/plex-3ds/cacert.pem";
        FILE* caF = fopen(CA_BUNDLE_PATH, "rb");
        if (caF) {
            fclose(caF);
            curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
        } else {
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        }

        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "User-Agent: Plex3DS/1.0");
        headers = curl_slist_append(headers, "Accept: */*");
        std::string clientId = m_clientIdentifier.empty() ? "Plex3DS-Client-001" : m_clientIdentifier;
        headers = curl_slist_append(headers, ("X-Plex-Client-Identifier: " + clientId).c_str());
        headers = curl_slist_append(headers, "X-Plex-Client-Profile-Name: Generic");
        if (current.item.type == MediaType::TRACK) {
            headers = curl_slist_append(headers, "X-Plex-Client-Profile-Extra: add-transcode-target(type=musicProfile&context=streaming&protocol=http&container=mp3&audioCodec=mp3)");
        } else {
            headers = curl_slist_append(headers, "X-Plex-Client-Profile-Extra: add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=h264&audioCodec=aac)");
        }
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Downloading " + current.item.title + "...";
        }

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        fclose(outFile);

        if (res == CURLE_OK && !m_cancelRequested.load()) {
            int64_t fileSize = 0;
            struct stat st;
            if (stat(finalPath.c_str(), &st) == 0) {
                fileSize = st.st_size;
            }

            if (fileSize > 4096) {
                saveMetadata(current.item, finalPath, fileSize);
                m_completedCount++;
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Saved: " + current.item.title;
            } else {
                remove(finalPath.c_str());
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Download failed (corrupt): " + current.item.title;
            }
        } else {
            remove(finalPath.c_str());
            if (m_cancelRequested.load()) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Cancelled";
                break;
            } else {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = std::string("Failed: ") + curl_easy_strerror(res);
                m_progress.failed = true;
            }
        }
    }

    m_isDownloading = false;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.active = false;
        m_progress.completed = !m_cancelRequested.load() && !m_progress.failed;
    }
}
#endif

bool DownloadManager::startDownload(const PlexMediaItem& item, const std::string& downloadUrl) {
    if (isDownloaded(item.ratingKey)) return false;
    std::vector<std::pair<PlexMediaItem, std::string>> list;
    list.push_back({item, downloadUrl});
    return queueDownloads(list) > 0;
}

int DownloadManager::queueDownloads(const std::vector<std::pair<PlexMediaItem, std::string>>& items) {
    std::vector<QueuedDownload> toAdd;
    for (const auto& pair : items) {
        if (!pair.first.ratingKey.empty() && !isDownloaded(pair.first.ratingKey)) {
            bool alreadyIn = false;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                if (m_isDownloading.load() && m_currentItem.ratingKey == pair.first.ratingKey) {
                    alreadyIn = true;
                } else {
                    for (const auto& q : m_queue) {
                        if (q.item.ratingKey == pair.first.ratingKey) {
                            alreadyIn = true;
                            break;
                        }
                    }
                }
            }
            if (!alreadyIn) {
                toAdd.push_back({pair.first, pair.second});
            }
        }
    }

    if (toAdd.empty()) return 0;
    int added = (int)toAdd.size();

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.insert(m_queue.end(), toAdd.begin(), toAdd.end());
        m_totalQueueCount += added;
    }

    if (!m_isDownloading.load()) {
        m_cancelRequested = false;
        m_isDownloading = true;
        {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.active = true;
            m_progress.statusText = "Starting queue (" + std::to_string(m_totalQueueCount) + " items)...";
            m_progress.completed = false;
            m_progress.failed = false;
        }

#ifdef __3DS__
        if (m_thread) {
            threadJoin(m_thread, U64_MAX);
            threadFree(m_thread);
            m_thread = nullptr;
        }
        m_thread = threadCreate(downloadThreadEntry, this, 128 * 1024, 0x33, -1, false);
        if (!m_thread) {
            m_thread = threadCreate(downloadThreadEntry, this, 128 * 1024, 0x33, -2, false);
        }
        if (!m_thread) {
            m_isDownloading = false;
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.active = false;
            return 0;
        }
#endif
    }
    return added;
}

void DownloadManager::cancelDownload() {
    m_cancelRequested = true;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_queue.clear();
        m_totalQueueCount = 0;
        m_currentQueueIndex = 0;
    }
#ifdef __3DS__
    if (m_thread) {
        threadJoin(m_thread, U64_MAX);
        threadFree(m_thread);
        m_thread = nullptr;
    }
#endif
    m_isDownloading = false;
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.active = false;
    }
}

DownloadProgress DownloadManager::getProgress() {
    std::lock_guard<std::mutex> lock(m_progressMutex);
    m_progress.completedCount = m_completedCount.load();
    return m_progress;
}

int DownloadManager::getQueueSize() {
    std::lock_guard<std::mutex> lock(m_queueMutex);
    return (int)m_queue.size() + (m_isDownloading.load() ? 1 : 0);
}
