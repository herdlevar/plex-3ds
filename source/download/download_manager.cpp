#include "download/download_manager.hpp"
#include "plex/plex_api.hpp"
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
#include <deque>

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
    std::string result = safe.substr(start, end - start);
    // Limit individual path components to 64 chars to prevent exceeding FAT32 260-char path limits
    if (result.length() > 64) {
        result = result.substr(0, 64);
        while (!result.empty() && (result.back() == ' ' || result.back() == '.' || result.back() == '_')) {
            result.pop_back();
        }
        if (result.empty()) result = "Unknown";
    }
    return result;
}

static bool isSafeDownloadPath(const std::string& p) {
    if (p.empty()) return false;
    if (p.rfind(BASE_DOWNLOAD_DIR + "/", 0) != 0) return false;
    if (p.find("..") != std::string::npos) return false;
    struct stat st;
    return (stat(p.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) && st.st_size > 4096);
}

static bool isSafeDownloadDir(const std::string& d) {
    if (d.empty()) return false;
    if (d.rfind(BASE_DOWNLOAD_DIR + "/", 0) != 0) return false;
    if (d.find("..") != std::string::npos) return false;
    if (d == MOVIES_DOWNLOAD_DIR || d == TV_DOWNLOAD_DIR || d == MUSIC_DOWNLOAD_DIR ||
        d == META_DOWNLOAD_DIR || d == LEGACY_VIDEO_DIR || d == LEGACY_MUSIC_DIR || d == BASE_DOWNLOAD_DIR) {
        return false;
    }
    return true;
}

static void removeFileAndPruneEmptyDirs(const std::string& path) {
    if (path.empty()) return;
    remove(path.c_str());
    size_t lastSlash = path.rfind('/');
    if (lastSlash != std::string::npos) {
        std::string parentDir = path.substr(0, lastSlash);
        if (isSafeDownloadDir(parentDir)) {
            rmdir(parentDir.c_str());
            size_t prevSlash = parentDir.rfind('/');
            if (prevSlash != std::string::npos) {
                std::string gpDir = parentDir.substr(0, prevSlash);
                if (isSafeDownloadDir(gpDir)) {
                    rmdir(gpDir.c_str());
                }
            }
        }
    }
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
    m_cacheDirty.store(true);
    return true;
}

void DownloadManager::exit() {
    cancelDownload();
}

void DownloadManager::invalidateCache() {
    m_cacheDirty.store(true);
}

void DownloadManager::refreshCache() {
    m_cacheDirty.store(true);
    refreshCacheInternal();
}

void DownloadManager::ensureCacheLoaded() const {
    if (m_cacheDirty.load()) {
        refreshCacheInternal();
    }
}

bool DownloadManager::isDownloaded(const std::string& ratingKey) const {
    if (ratingKey.empty()) return false;
    ensureCacheLoaded();
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cachedRatingKeys.count(ratingKey) > 0;
}

bool DownloadManager::isQueued(const std::string& ratingKey) const {
    if (ratingKey.empty()) return false;
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_queueMutex));
    if (m_isDownloading.load() && m_currentItem.ratingKey == ratingKey) {
        return true;
    }
    for (const auto& q : m_queue) {
        if (q.item.ratingKey == ratingKey) {
            return true;
        }
    }
    return false;
}

int DownloadManager::getQueuePosition(const std::string& ratingKey) const {
    if (ratingKey.empty()) return 0;
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(m_queueMutex));
    if (m_isDownloading.load() && m_currentItem.ratingKey == ratingKey) {
        return 1;
    }
    for (size_t i = 0; i < m_queue.size(); i++) {
        if (m_queue[i].item.ratingKey == ratingKey) {
            return (m_isDownloading.load() ? 2 : 1) + (int)i;
        }
    }
    return 0;
}

std::string DownloadManager::getLocalFilePath(const std::string& ratingKey) const {
    if (ratingKey.empty()) return "";
    ensureCacheLoaded();
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    auto it = m_cachedPaths.find(ratingKey);
    if (it != m_cachedPaths.end()) {
        return it->second;
    }
    return "";
}

std::vector<PlexMediaItem> DownloadManager::getDownloadedItems() {
    ensureCacheLoaded();
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    return m_cachedDownloadedItems;
}

void DownloadManager::refreshCacheInternal() const {
    std::lock_guard<std::mutex> lock(m_cacheMutex);
    if (!m_cacheDirty.load()) return;

    std::vector<PlexMediaItem> items;
    std::unordered_map<std::string, std::string> paths;
    std::unordered_set<std::string> ratingKeys;
    std::unordered_set<std::string> knownFilePaths;

    DIR* dir = opendir(META_DOWNLOAD_DIR.c_str());
    if (dir) {
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

                if (sz > 0) {
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
                            return (stat(path.c_str(), &st) == 0 && !S_ISDIR(st.st_mode) && st.st_size > 4096);
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

                            if (!it.ratingKey.empty()) {
                                ratingKeys.insert(it.ratingKey);
                                paths[it.ratingKey] = lp;
                            }
                            knownFilePaths.insert(lp);
                            items.push_back(it);
                        }
                        cJSON_Delete(root);
                    }
                } else {
                    fclose(f);
                }
            }
        }
        closedir(dir);
    }

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

                if (knownFilePaths.count(fullPath) > 0) continue; // Fast O(1) set lookup

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
                if (!it.ratingKey.empty()) {
                    ratingKeys.insert(it.ratingKey);
                    paths[it.ratingKey] = fullPath;
                }
                knownFilePaths.insert(fullPath);
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

    m_cachedDownloadedItems = std::move(items);
    m_cachedPaths = std::move(paths);
    m_cachedRatingKeys = std::move(ratingKeys);
    m_cacheDirty.store(false);
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

    if (!localPath.empty() && isSafeDownloadPath(localPath)) {
        removeFileAndPruneEmptyDirs(localPath);
    }
    remove(metaPath.c_str());

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        m_cachedRatingKeys.erase(ratingKey);
        m_cachedPaths.erase(ratingKey);
        for (auto it = m_cachedDownloadedItems.begin(); it != m_cachedDownloadedItems.end(); ++it) {
            if (it->ratingKey == ratingKey) {
                m_cachedDownloadedItems.erase(it);
                break;
            }
        }
    }
    m_cacheDirty.store(true);
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

    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (auto& it : m_cachedDownloadedItems) {
            if (it.ratingKey == ratingKey) {
                it.viewOffsetMs = offsetMs;
                break;
            }
        }
    }
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

    PlexMediaItem it = item;
    it.isOffline = true;
    it.localFilePath = filePath;
    it.partKey = filePath;
    it.key = filePath;
    it.localFileSize = fileSize;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        if (!it.ratingKey.empty()) {
            m_cachedRatingKeys.insert(it.ratingKey);
            m_cachedPaths[it.ratingKey] = filePath;
            bool found = false;
            for (auto& existing : m_cachedDownloadedItems) {
                if (existing.ratingKey == it.ratingKey) {
                    existing = it;
                    found = true;
                    break;
                }
            }
            if (!found) {
                m_cachedDownloadedItems.push_back(it);
            }
        }
    }
    m_cacheDirty.store(true);
}

#ifdef __3DS__
void DownloadManager::downloadThreadEntry(void* arg) {
    DownloadManager* self = static_cast<DownloadManager*>(arg);
    if (self) self->downloadLoop();
}

static constexpr size_t NUM_STAGING_BLOCKS = 16;
static constexpr size_t STAGING_BLOCK_SIZE = 64 * 1024; // 64 KB (matches SD FAT32 cluster size)

struct alignas(32) StagingBlock {
    size_t size = 0;
    uint8_t data[STAGING_BLOCK_SIZE];
};

struct DownloadContext {
    DownloadManager* mgr = nullptr;
    FILE* fp = nullptr;

    std::mutex queueMutex;
    std::deque<StagingBlock*> freeBlocks;
    std::deque<StagingBlock*> writeQueue;
    StagingBlock* activeBlock = nullptr;

    std::atomic<bool> downloadFinished{false};
    std::atomic<bool> writerError{false};
    bool useWriterThread = false;
    Thread writerThread = nullptr;
};

void DownloadManager::writerThreadEntry(void* arg) {
    DownloadContext* dc = static_cast<DownloadContext*>(arg);
    if (!dc) return;

    while (true) {
        if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) {
            break;
        }

        while (g_isSuspended.load() && !dc->mgr->m_cancelRequested.load() && !g_appExiting.load()) {
            svcSleepThread(50000000); // 50ms pause during Home Menu
        }

        StagingBlock* blockToWrite = nullptr;
        {
            std::lock_guard<std::mutex> lock(dc->queueMutex);
            if (!dc->writeQueue.empty()) {
                blockToWrite = dc->writeQueue.front();
                dc->writeQueue.pop_front();
            }
        }

        if (blockToWrite) {
            if (blockToWrite->size > 0 && dc->fp) {
                size_t written = fwrite(blockToWrite->data, 1, blockToWrite->size, dc->fp);
                if (written != blockToWrite->size) {
                    dc->writerError.store(true);
                }
            }

            {
                std::lock_guard<std::mutex> lock(dc->queueMutex);
                blockToWrite->size = 0;
                dc->freeBlocks.push_back(blockToWrite);
            }
            continue; // Keep flushing blocks without sleeping
        }

        // writeQueue is empty
        if (dc->downloadFinished.load()) {
            std::lock_guard<std::mutex> lock(dc->queueMutex);
            if (dc->writeQueue.empty()) {
                break; // Complete
            }
            continue;
        }

        // Sleep 2ms waiting for network thread to fill a block
        svcSleepThread(2000000); // 2ms
    }
}

void DownloadManager::downloadLoop() {
    ensureDirectories();

    // Pre-allocate 1 MB staging reservoir (16 x 64 KB blocks) for decoupled SD writes
    std::vector<StagingBlock> blockStorage(NUM_STAGING_BLOCKS);

    while (!m_cancelRequested.load() && !g_appExiting.load()) {
        QueuedDownload current;
        int qIdx = 0;
        int qTot = 0;
        {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            if (m_queue.empty()) {
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

        std::string downloadUrl = current.url;
        // JIT (Just-In-Time) transcode URL resolution:
        // Ephemeral transcode sessions in Plex expire within 15-30 seconds if not connected immediately.
        // If a server is provided and the URL is empty (or has an ephemeral transcode session),
        // resolve a fresh transcode decision and stream URL right now.
        if (!current.server.selectedUri.empty() && (downloadUrl.empty() || downloadUrl.find("/transcode/universal/") != std::string::npos)) {
            PlexAPI api(m_clientIdentifier.empty() ? current.config.clientIdentifier : m_clientIdentifier);
            downloadUrl = api.buildTranscodeUrl(current.server, current.item, current.config);
        }

        if (downloadUrl.empty()) {
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Failed to resolve download URL: " + current.item.title;
            m_progress.failed = true;
            continue;
        }
        m_currentUrl = downloadUrl;

        std::string finalPath = buildLocalMediaPath(current.item);

        FILE* outFile = fopen(finalPath.c_str(), "wb");
        if (!outFile) {
            removeFileAndPruneEmptyDirs(finalPath);
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Cannot create file on SD";
            m_progress.failed = true;
            continue;
        }

        // Buffer SD writes in 64KB chunks to align with FAT32 clusters and minimize SDMC IPC overhead
        std::vector<char> fileBuffer(64 * 1024);
        setvbuf(outFile, fileBuffer.data(), _IOFBF, fileBuffer.size());

        CURL* curl = curl_easy_init();
        if (!curl) {
            fclose(outFile);
            removeFileAndPruneEmptyDirs(finalPath);
            std::lock_guard<std::mutex> lock(m_progressMutex);
            m_progress.statusText = "Failed to initialize curl";
            m_progress.failed = true;
            break;
        }

        DownloadContext ctx;
        ctx.mgr = this;
        ctx.fp = outFile;
        ctx.downloadFinished.store(false);
        ctx.writerError.store(false);
        ctx.useWriterThread = false;
        ctx.writerThread = nullptr;
        ctx.activeBlock = nullptr;
        ctx.freeBlocks.clear();
        ctx.writeQueue.clear();

        for (size_t i = 0; i < NUM_STAGING_BLOCKS; ++i) {
            blockStorage[i].size = 0;
            ctx.freeBlocks.push_back(&blockStorage[i]);
        }

        ctx.writerThread = threadCreate(writerThreadEntry, &ctx, 64 * 1024, 0x33, -1, false);
        if (!ctx.writerThread) {
            ctx.writerThread = threadCreate(writerThreadEntry, &ctx, 64 * 1024, 0x33, -2, false);
        }
        if (ctx.writerThread) {
            ctx.useWriterThread = true;
        }

        auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
            size_t totalBytes = size * nmemb;
            if (totalBytes == 0) return 0;
            DownloadContext* dc = static_cast<DownloadContext*>(userdata);

            if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) return 0;
            while (g_isSuspended.load() && !dc->mgr->m_cancelRequested.load() && !g_appExiting.load()) {
                svcSleepThread(50000000); // 50ms pause during Home Menu
            }
            if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) return 0;

            if (!dc->useWriterThread) {
                // Synchronous fallback
                return fwrite(ptr, 1, totalBytes, dc->fp);
            }

            const uint8_t* src = static_cast<const uint8_t*>(ptr);
            size_t bytesLeft = totalBytes;

            while (bytesLeft > 0) {
                if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) {
                    return 0;
                }

                while (g_isSuspended.load() && !dc->mgr->m_cancelRequested.load() && !g_appExiting.load()) {
                    svcSleepThread(50000000); // 50ms pause during Home Menu
                }
                if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) {
                    return 0;
                }

                if (!dc->activeBlock) {
                    while (true) {
                        {
                            std::lock_guard<std::mutex> lock(dc->queueMutex);
                            if (!dc->freeBlocks.empty()) {
                                dc->activeBlock = dc->freeBlocks.front();
                                dc->freeBlocks.pop_front();
                                dc->activeBlock->size = 0;
                                break;
                            }
                        }
                        if (dc->mgr->m_cancelRequested.load() || g_appExiting.load() || dc->writerError.load()) {
                            return 0;
                        }
                        svcSleepThread(2000000); // 2ms wait for writer thread to free a block
                    }
                }

                size_t space = STAGING_BLOCK_SIZE - dc->activeBlock->size;
                size_t toCopy = std::min(bytesLeft, space);
                std::memcpy(dc->activeBlock->data + dc->activeBlock->size, src, toCopy);
                dc->activeBlock->size += toCopy;
                src += toCopy;
                bytesLeft -= toCopy;

                if (dc->activeBlock->size >= STAGING_BLOCK_SIZE) {
                    {
                        std::lock_guard<std::mutex> lock(dc->queueMutex);
                        dc->writeQueue.push_back(dc->activeBlock);
                    }
                    dc->activeBlock = nullptr;
                }
            }

            return totalBytes;
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

        curl_easy_setopt(curl, CURLOPT_URL, downloadUrl.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)+writeCb);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, (curl_xferinfo_callback)+xferInfoCb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
        curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
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
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        bool httpOk = (httpCode >= 200 && httpCode < 300);

        if (ctx.useWriterThread) {
            // Push any remaining partial block (< 64 KB) to the write queue
            if (ctx.activeBlock && ctx.activeBlock->size > 0 &&
                !m_cancelRequested.load() && !g_appExiting.load() && !ctx.writerError.load()) {
                std::lock_guard<std::mutex> lock(ctx.queueMutex);
                ctx.writeQueue.push_back(ctx.activeBlock);
                ctx.activeBlock = nullptr;
            }
            ctx.downloadFinished.store(true);

            if (ctx.writerThread) {
                threadJoin(ctx.writerThread, U64_MAX);
                threadFree(ctx.writerThread);
                ctx.writerThread = nullptr;
            }
        }

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        fflush(outFile);
        fclose(outFile);

        if (res == CURLE_OK && httpOk && !m_cancelRequested.load() && !ctx.writerError.load()) {
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
                removeFileAndPruneEmptyDirs(finalPath);
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Download failed (corrupt/incomplete): " + current.item.title;
                m_progress.failed = true;
            }
        } else {
            removeFileAndPruneEmptyDirs(finalPath);
            if (m_cancelRequested.load()) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Cancelled";
                break;
            } else if (ctx.writerError.load()) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Failed: SD card write error";
                m_progress.failed = true;
            } else if (res == CURLE_OK && !httpOk) {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = "Download failed (HTTP " + std::to_string(httpCode) + "): " + current.item.title;
                m_progress.failed = true;
            } else {
                std::lock_guard<std::mutex> lock(m_progressMutex);
                m_progress.statusText = std::string("Failed: ") + curl_easy_strerror(res);
                m_progress.failed = true;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        m_isDownloading = false;
        m_totalQueueCount = 0;
        m_currentQueueIndex = 0;
        m_currentItem = PlexMediaItem();
    }
    {
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.active = false;
        m_progress.completed = !m_cancelRequested.load() && !m_progress.failed;
    }
}
#endif

bool DownloadManager::startDownload(const PlexMediaItem& item, const std::string& downloadUrl,
                                   const PlexServer& server, const AppConfig& config) {
    if (isDownloaded(item.ratingKey)) return false;
    std::vector<std::pair<PlexMediaItem, std::string>> list;
    list.push_back({item, downloadUrl});
    return queueDownloads(list, server, config) > 0;
}

int DownloadManager::queueDownloads(const std::vector<std::pair<PlexMediaItem, std::string>>& items,
                                   const PlexServer& server,
                                   const AppConfig& config) {
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
                toAdd.push_back({pair.first, pair.second, server, config});
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
    } else {
        // Already downloading: update progress queueCount immediately so UI reflects it on next frame
        std::lock_guard<std::mutex> lock(m_progressMutex);
        m_progress.queueCount = m_totalQueueCount;
        if (m_progress.active) {
            std::string qInfo = "(" + std::to_string(m_progress.queueIndex) + "/" + std::to_string(m_totalQueueCount) + ") ";
            if (m_progress.totalBytes > 0) {
                double mbNow = (double)m_progress.bytesDownloaded / (1024.0 * 1024.0);
                double mbTot = (double)m_progress.totalBytes / (1024.0 * 1024.0);
                char buf[64];
                snprintf(buf, sizeof(buf), "%.1f / %.1f MB (%d%%)", mbNow, mbTot, m_progress.percent);
                m_progress.statusText = qInfo + buf;
            }
        }
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

bool DownloadManager::cancelQueuedItem(const std::string& ratingKey) {
    if (ratingKey.empty()) return false;
    std::lock_guard<std::mutex> lock(m_queueMutex);
    for (auto it = m_queue.begin(); it != m_queue.end(); ++it) {
        if (it->item.ratingKey == ratingKey) {
            m_queue.erase(it);
            if (m_totalQueueCount > 0) m_totalQueueCount--;
            {
                std::lock_guard<std::mutex> pLock(m_progressMutex);
                m_progress.queueCount = m_totalQueueCount;
            }
            return true;
        }
    }
    if (m_isDownloading.load() && m_currentItem.ratingKey == ratingKey) {
        cancelDownload();
        return true;
    }
    return false;
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
