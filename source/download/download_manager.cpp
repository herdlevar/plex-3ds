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

static const std::string BASE_DOWNLOAD_DIR = "sdmc:/3ds/plex-3ds/downloads";
static const std::string VIDEO_DOWNLOAD_DIR = "sdmc:/3ds/plex-3ds/downloads/videos";
static const std::string MUSIC_DOWNLOAD_DIR = "sdmc:/3ds/plex-3ds/downloads/music";
static const std::string META_DOWNLOAD_DIR  = "sdmc:/3ds/plex-3ds/downloads/meta";

static std::string sanitizeKey(const std::string& key) {
    std::string safe;
    for (char c : key) {
        if (isalnum((unsigned char)c) || c == '_' || c == '-') {
            safe += c;
        }
    }
    return safe.empty() ? "item" : safe;
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
    mkdir(VIDEO_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(MUSIC_DOWNLOAD_DIR.c_str(), 0777);
    mkdir(META_DOWNLOAD_DIR.c_str(), 0777);
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
    if (f) {
        fclose(f);
        return true;
    }
    return false;
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

                std::string lp = (localPath && localPath->valuestring) ? localPath->valuestring : "";
                // Verify local media file exists
                FILE* mediaFile = fopen(lp.c_str(), "rb");
                if (mediaFile) {
                    fclose(mediaFile);

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
                    else it.type = MediaType::MOVIE;

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
    cJSON_AddStringToObject(root, "type", (item.type == MediaType::TRACK ? "track" : "movie"));
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

    std::string safeKey = sanitizeKey(m_currentItem.ratingKey);
    std::string ext = (m_currentItem.type == MediaType::TRACK) ? ".mp3" : ".mkv";
    std::string folder = (m_currentItem.type == MediaType::TRACK) ? MUSIC_DOWNLOAD_DIR : VIDEO_DOWNLOAD_DIR;
    std::string finalPath = folder + "/" + safeKey + ext;
    std::string tempPath = BASE_DOWNLOAD_DIR + "/temp_" + safeKey + ext;

    FILE* outFile = fopen(tempPath.c_str(), "wb");
    if (!outFile) {
        m_progress.statusText = "Cannot create file on SD";
        m_progress.failed = true;
        m_isDownloading = false;
        return;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        fclose(outFile);
        remove(tempPath.c_str());
        m_progress.statusText = "Failed to initialize curl";
        m_progress.failed = true;
        m_isDownloading = false;
        return;
    }

    struct DownloadContext {
        DownloadManager* mgr;
        FILE* fp;
    } ctx = { this, outFile };

    auto writeCb = [](void* ptr, size_t size, size_t nmemb, void* userdata) -> size_t {
        DownloadContext* dc = (DownloadContext*)userdata;
        if (dc->mgr->m_cancelRequested.load() || g_appExiting.load()) return 0;
        return fwrite(ptr, size, nmemb, dc->fp);
    };

    auto xferInfoCb = [](void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) -> int {
        (void)ultotal; (void)ulnow;
        DownloadManager* mgr = (DownloadManager*)clientp;
        if (mgr->m_cancelRequested.load() || g_appExiting.load()) return 1;

        mgr->m_progress.bytesDownloaded = (int64_t)dlnow;
        mgr->m_progress.totalBytes = (int64_t)dltotal;
        if (dltotal > 0) {
            mgr->m_progress.percent = (int)((dlnow * 100) / dltotal);
            int mbNow = (int)(dlnow / (1024 * 1024));
            int mbTot = (int)(dltotal / (1024 * 1024));
            mgr->m_progress.statusText = std::to_string(mbNow) + " / " + std::to_string(mbTot) + " MB (" + std::to_string(mgr->m_progress.percent) + "%)";
        } else {
            int mbNow = (int)(dlnow / (1024 * 1024));
            mgr->m_progress.statusText = std::to_string(mbNow) + " MB downloaded";
        }
        return 0;
    };

    curl_easy_setopt(curl, CURLOPT_URL, m_currentUrl.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, (curl_write_callback)+writeCb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, (curl_xferinfo_callback)+xferInfoCb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 65536L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);

    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: Plex3DS/1.0");
    headers = curl_slist_append(headers, "Accept: */*");
    headers = curl_slist_append(headers, "X-Plex-Client-Identifier: Plex3DS-Client-001");
    headers = curl_slist_append(headers, "X-Plex-Client-Profile-Name: Generic");
    headers = curl_slist_append(headers, "X-Plex-Client-Profile-Extra: add-transcode-target(type=videoProfile&context=streaming&protocol=http&container=mkv&videoCodec=h264&audioCodec=aac)");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    m_progress.statusText = "Downloading...";
    CURLcode res = curl_easy_perform(curl);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(outFile);

    if (res == CURLE_OK && !m_cancelRequested.load()) {
        remove(finalPath.c_str());
        rename(tempPath.c_str(), finalPath.c_str());

        int64_t fileSize = 0;
        struct stat st;
        if (stat(finalPath.c_str(), &st) == 0) {
            fileSize = st.st_size;
        }

        saveMetadata(m_currentItem, finalPath, fileSize);
        m_progress.statusText = "Download complete!";
        m_progress.completed = true;
    } else {
        remove(tempPath.c_str());
        if (m_cancelRequested.load()) {
            m_progress.statusText = "Cancelled";
        } else {
            m_progress.statusText = std::string("Failed: ") + curl_easy_strerror(res);
        }
        m_progress.failed = true;
    }

    m_isDownloading = false;
}
#endif

bool DownloadManager::startDownload(const PlexMediaItem& item, const std::string& downloadUrl) {
    if (m_isDownloading.load()) return false;

    m_currentItem = item;
    m_currentUrl = downloadUrl;
    m_cancelRequested = false;
    m_isDownloading = true;

    m_progress.active = true;
    m_progress.ratingKey = item.ratingKey;
    m_progress.title = item.title;
    m_progress.bytesDownloaded = 0;
    m_progress.totalBytes = 0;
    m_progress.percent = 0;
    m_progress.statusText = "Connecting...";
    m_progress.completed = false;
    m_progress.failed = false;

#ifdef __3DS__
    if (m_thread) {
        threadJoin(m_thread, U64_MAX);
        threadFree(m_thread);
        m_thread = nullptr;
    }
    m_thread = threadCreate(downloadThreadEntry, this, 64 * 1024, 0x31, -2, false);
    if (!m_thread) {
        m_isDownloading = false;
        m_progress.active = false;
        return false;
    }
#endif
    return true;
}

void DownloadManager::cancelDownload() {
    m_cancelRequested = true;
#ifdef __3DS__
    if (m_thread) {
        threadJoin(m_thread, U64_MAX);
        threadFree(m_thread);
        m_thread = nullptr;
    }
#endif
    m_isDownloading = false;
    m_progress.active = false;
}

DownloadProgress DownloadManager::getProgress() {
    return m_progress;
}
