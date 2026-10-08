#include "types.hpp"
#include "network/http.hpp"
#include "plex/plex_api.hpp"
#include "ui/ui.hpp"
#include "player/audio_player.hpp"
#include "player/video_player.hpp"
#include "download/download_manager.hpp"
#include "cJSON.h"

#ifdef __3DS__
#include <3ds.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <vector>
#include <set>
#include <memory>
#include <algorithm>

static AppConfig g_config;
static AppState g_state = AppState::PIN_AUTH;
static std::string g_pinId;
static std::string g_pinCode;
static uint64_t g_lastPollTime = 0;

static std::vector<PlexServer> g_servers;
static int g_selectedServerIdx = 0;

static std::vector<PlexLibrary> g_libraries;
static int g_selectedLibraryIdx = 0;

static std::vector<PlexMediaItem> g_items;
static int g_selectedItemIdx = 0;
static int g_scrollOffset = 0;

struct NavHistory {
    std::vector<PlexMediaItem> items;
    std::string title;
    std::string navKey;
    int selectedIdx;
    int scrollOffset;
};
static std::vector<NavHistory> g_navStack;
static std::string g_currentNavTitle = "Media";
static std::string g_currentNavKey = "";
static std::string g_statusMsg = "Connecting...";

static PlexMediaItem g_nowPlayingItem;
static bool g_hasNowPlaying = false;
static bool g_controlsExpanded = false;
static bool g_isScrubbing = false;
static int g_scrubSec = 0;

static std::vector<PlexMediaItem> g_playlistItems;
static int g_playlistIndex = -1;

static DownloadManager g_downloadManager;

std::atomic<bool> g_appExiting{false};
std::atomic<bool> g_isSuspended{false};
std::atomic<bool> g_gpuRightLost{false};

#ifdef __3DS__
static aptHookCookie g_aptCookie;
static bool g_videoWasPlayingOnSuspend = false;
static bool g_audioWasPlayingOnSuspend = false;
static std::atomic<bool> g_needsPostWakeupResume{false};

static bool s_bottomScreenOff = false;
static uint64_t s_lastUserActivityTime = 0;
static bool s_ignoringTouchUntilRelease = false;

static bool s_shellClosed = false;
static bool s_lastHeadphoneStatus = false;

static AudioPlayer* g_pAudioPlayer = nullptr;
static VideoPlayer* g_pVideoPlayer = nullptr;

static inline bool isHeadphoneConnected() {
    bool inserted = false;
    if (R_SUCCEEDED(DSP_GetHeadphoneStatus(&inserted))) {
        s_lastHeadphoneStatus = inserted;
        return inserted;
    }
    return s_lastHeadphoneStatus;
}

static void onAptHook(APT_HookType hook, void* param) {
    (void)param;
    switch (hook) {
        case APTHOOK_ONSUSPEND:
        case APTHOOK_ONSLEEP:
            g_gpuRightLost = true;
            g_isSuspended = true;
            if (g_pVideoPlayer && g_pVideoPlayer->isPlaying()) {
                if (!g_pVideoPlayer->isPaused()) {
                    g_videoWasPlayingOnSuspend = true;
                }
                g_pVideoPlayer->suspend();
            }
            if (g_pAudioPlayer && g_pAudioPlayer->isPlaying()) {
                if (!g_pAudioPlayer->isPaused()) {
                    g_audioWasPlayingOnSuspend = true;
                }
                g_pAudioPlayer->suspend();
            }
            break;

        case APTHOOK_ONRESTORE:
        case APTHOOK_ONWAKEUP:
            // CRITICAL: Do NOT access DSP/NDSP or GSP/LCD services here!
            // In libctru, APTHOOK_ONRESTORE is called before hardware services finish re-synchronization.
            // Hardware and display resume is deferred to the main loop once aptMainLoop() returns.
            g_gpuRightLost = false;
            s_shellClosed = false;
            s_lastUserActivityTime = osGetTime();
            g_isSuspended = false;
            g_needsPostWakeupResume = true;
            break;

        case APTHOOK_ONEXIT:
            g_appExiting = true;
            g_gpuRightLost = true;
            g_downloadManager.cancelDownload();
            if (g_pVideoPlayer) g_pVideoPlayer->stop();
            if (g_pAudioPlayer) g_pAudioPlayer->stop();
            break;

        default:
            break;
    }
}
#endif


static void ensureOfflineLibrary() {
    for (const auto& lib : g_libraries) {
        if (lib.key == "__offline__") return;
    }
    PlexLibrary offLib;
    offLib.key = "__offline__";
    offLib.title = "Downloads (Offline)";
    offLib.type = "offline";
    g_libraries.push_back(offLib);
}

static std::map<std::string, int64_t> g_resumeMap;

static void loadResume() {
    FILE* f = fopen("sdmc:/3ds/plex-3ds/resume.json", "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::string content(sz, '\0');
    fread(&content[0], 1, sz, f);
    fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return;

    cJSON* child = root->child;
    while (child) {
        if (child->string && cJSON_IsNumber(child)) {
            g_resumeMap[child->string] = (int64_t)child->valuedouble;
        }
        child = child->next;
    }
    cJSON_Delete(root);
}

static void saveResume() {
    cJSON* root = cJSON_CreateObject();
    for (const auto& kv : g_resumeMap) {
        if (kv.second > 10000) {
            cJSON_AddNumberToObject(root, kv.first.c_str(), (double)kv.second);
        }
    }
    char* jsonStr = cJSON_Print(root);
    if (jsonStr) {
#ifdef __3DS__
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/plex-3ds", 0777);
#endif
        FILE* f = fopen("sdmc:/3ds/plex-3ds/resume.json", "wb");
        if (f) {
            fputs(jsonStr, f);
            fclose(f);
        }
        free(jsonStr);
    }
    cJSON_Delete(root);
}

static AppState s_downloadsReturnState = AppState::SERVER_SELECT;

static std::vector<PlexMediaItem> buildOfflineShows(const std::vector<PlexMediaItem>& episodes) {
    std::map<std::string, std::vector<PlexMediaItem>> showMap;
    for (const auto& ep : episodes) {
        std::string s = !ep.grandparentTitle.empty() ? ep.grandparentTitle : (!ep.parentTitle.empty() ? ep.parentTitle : "Unknown Show");
        showMap[s].push_back(ep);
    }

    std::vector<PlexMediaItem> showItems;
    for (const auto& pair : showMap) {
        std::set<std::string> seasons;
        for (const auto& ep : pair.second) {
            std::string szn = !ep.parentTitle.empty() ? ep.parentTitle : "Season 01";
            seasons.insert(szn);
        }
        PlexMediaItem it;
        it.title = pair.first;
        it.type = MediaType::SHOW;
        it.key = "__offline_show:" + pair.first;
        it.isOffline = true;
        it.summary = std::to_string(seasons.size()) + " season" + (seasons.size() == 1 ? "" : "s") + ", " +
                     std::to_string(pair.second.size()) + " episode" + (pair.second.size() == 1 ? "" : "s");
        showItems.push_back(it);
    }
    std::sort(showItems.begin(), showItems.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        return a.title < b.title;
    });
    return showItems;
}

static std::vector<PlexMediaItem> buildOfflineSeasons(const std::vector<PlexMediaItem>& episodes, const std::string& targetShow) {
    std::map<std::string, std::vector<PlexMediaItem>> seasonMap;
    for (const auto& ep : episodes) {
        std::string s = !ep.grandparentTitle.empty() ? ep.grandparentTitle : (!ep.parentTitle.empty() ? ep.parentTitle : "Unknown Show");
        if (s == targetShow) {
            std::string szn = !ep.parentTitle.empty() ? ep.parentTitle : "Season 01";
            seasonMap[szn].push_back(ep);
        }
    }

    std::vector<PlexMediaItem> seasonItems;
    for (const auto& pair : seasonMap) {
        PlexMediaItem it;
        it.title = pair.first;
        it.parentTitle = targetShow;
        it.type = MediaType::SEASON;
        it.key = "__offline_season:" + targetShow + "/" + pair.first;
        it.isOffline = true;
        it.summary = std::to_string(pair.second.size()) + " episode" + (pair.second.size() == 1 ? "" : "s");
        seasonItems.push_back(it);
    }
    std::sort(seasonItems.begin(), seasonItems.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        return a.title < b.title;
    });
    return seasonItems;
}

static std::vector<PlexMediaItem> buildOfflineEpisodes(const std::vector<PlexMediaItem>& episodes, const std::string& targetShow, const std::string& targetSeason) {
    std::vector<PlexMediaItem> result;
    for (const auto& ep : episodes) {
        std::string s = !ep.grandparentTitle.empty() ? ep.grandparentTitle : (!ep.parentTitle.empty() ? ep.parentTitle : "Unknown Show");
        std::string szn = !ep.parentTitle.empty() ? ep.parentTitle : "Season 01";
        if (s == targetShow && szn == targetSeason) {
            result.push_back(ep);
        }
    }
    std::sort(result.begin(), result.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        if (a.index != b.index && a.index > 0 && b.index > 0) return a.index < b.index;
        return a.title < b.title;
    });
    return result;
}

static std::vector<PlexMediaItem> buildOfflineArtists(const std::vector<PlexMediaItem>& tracks) {
    std::map<std::string, std::vector<PlexMediaItem>> artistMap;
    for (const auto& trk : tracks) {
        std::string a = !trk.grandparentTitle.empty() ? trk.grandparentTitle : "Unknown Artist";
        artistMap[a].push_back(trk);
    }

    std::vector<PlexMediaItem> artistItems;
    for (const auto& pair : artistMap) {
        std::set<std::string> albums;
        for (const auto& trk : pair.second) {
            std::string alb = !trk.parentTitle.empty() ? trk.parentTitle : "Unknown Album";
            albums.insert(alb);
        }
        PlexMediaItem it;
        it.title = pair.first;
        it.type = MediaType::ARTIST;
        it.key = "__offline_artist:" + pair.first;
        it.isOffline = true;
        it.summary = std::to_string(albums.size()) + " album" + (albums.size() == 1 ? "" : "s") + ", " +
                     std::to_string(pair.second.size()) + " track" + (pair.second.size() == 1 ? "" : "s");
        artistItems.push_back(it);
    }
    std::sort(artistItems.begin(), artistItems.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        return a.title < b.title;
    });
    return artistItems;
}

static std::vector<PlexMediaItem> buildOfflineAlbums(const std::vector<PlexMediaItem>& tracks, const std::string& targetArtist) {
    std::map<std::string, std::vector<PlexMediaItem>> albumMap;
    for (const auto& trk : tracks) {
        std::string a = !trk.grandparentTitle.empty() ? trk.grandparentTitle : "Unknown Artist";
        if (a == targetArtist) {
            std::string alb = !trk.parentTitle.empty() ? trk.parentTitle : "Unknown Album";
            albumMap[alb].push_back(trk);
        }
    }

    std::vector<PlexMediaItem> albumItems;
    for (const auto& pair : albumMap) {
        PlexMediaItem it;
        it.title = pair.first;
        it.parentTitle = targetArtist;
        it.type = MediaType::ALBUM;
        it.key = "__offline_album:" + targetArtist + "/" + pair.first;
        it.isOffline = true;
        int albumYear = 0;
        for (const auto& trk : pair.second) {
            if (trk.year > 0) { albumYear = trk.year; break; }
        }
        it.year = albumYear;
        it.summary = std::to_string(pair.second.size()) + " track" + (pair.second.size() == 1 ? "" : "s");
        albumItems.push_back(it);
    }
    std::sort(albumItems.begin(), albumItems.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        return a.title < b.title;
    });
    return albumItems;
}

static std::vector<PlexMediaItem> buildOfflineTracks(const std::vector<PlexMediaItem>& tracks, const std::string& targetArtist, const std::string& targetAlbum) {
    std::vector<PlexMediaItem> result;
    for (const auto& trk : tracks) {
        std::string a = !trk.grandparentTitle.empty() ? trk.grandparentTitle : "Unknown Artist";
        std::string alb = !trk.parentTitle.empty() ? trk.parentTitle : "Unknown Album";
        if (a == targetArtist && alb == targetAlbum) {
            result.push_back(trk);
        }
    }
    std::sort(result.begin(), result.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
        if (a.index != b.index && a.index > 0 && b.index > 0) return a.index < b.index;
        return a.title < b.title;
    });
    return result;
}

static std::vector<PlexMediaItem> buildOfflineCategories(const std::vector<PlexMediaItem>& allDownloads) {
    std::vector<PlexMediaItem> movies;
    std::vector<PlexMediaItem> episodes;
    std::vector<PlexMediaItem> tracks;

    for (const auto& it : allDownloads) {
        if (it.type == MediaType::TRACK) {
            tracks.push_back(it);
        } else if (it.type == MediaType::EPISODE) {
            episodes.push_back(it);
        } else {
            movies.push_back(it);
        }
    }

    int catCount = (movies.empty() ? 0 : 1) + (episodes.empty() ? 0 : 1) + (tracks.empty() ? 0 : 1);
    if (catCount == 0) {
        return {};
    }

    if (catCount == 1) {
        if (!movies.empty()) {
            std::sort(movies.begin(), movies.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
                return a.title < b.title;
            });
            return movies;
        } else if (!episodes.empty()) {
            return buildOfflineShows(episodes);
        } else {
            return buildOfflineArtists(tracks);
        }
    }

    std::vector<PlexMediaItem> categories;
    if (!movies.empty()) {
        PlexMediaItem cat;
        cat.title = "Movies (" + std::to_string(movies.size()) + ")";
        cat.type = MediaType::SHOW;
        cat.key = "__offline_movies__";
        cat.isOffline = true;
        cat.summary = std::to_string(movies.size()) + " downloaded movie" + (movies.size() == 1 ? "" : "s");
        categories.push_back(cat);
    }
    if (!episodes.empty()) {
        std::set<std::string> uniqueShows;
        for (const auto& ep : episodes) {
            std::string s = !ep.grandparentTitle.empty() ? ep.grandparentTitle : (!ep.parentTitle.empty() ? ep.parentTitle : "Unknown Show");
            uniqueShows.insert(s);
        }
        PlexMediaItem cat;
        cat.title = "TV Shows (" + std::to_string(uniqueShows.size()) + ")";
        cat.type = MediaType::SHOW;
        cat.key = "__offline_tv__";
        cat.isOffline = true;
        cat.summary = std::to_string(episodes.size()) + " episode" + (episodes.size() == 1 ? "" : "s") + " across " + std::to_string(uniqueShows.size()) + " show" + (uniqueShows.size() == 1 ? "" : "s");
        categories.push_back(cat);
    }
    if (!tracks.empty()) {
        std::set<std::string> uniqueArtists;
        for (const auto& trk : tracks) {
            std::string a = !trk.grandparentTitle.empty() ? trk.grandparentTitle : "Unknown Artist";
            uniqueArtists.insert(a);
        }
        PlexMediaItem cat;
        cat.title = "Music (" + std::to_string(uniqueArtists.size()) + ")";
        cat.type = MediaType::ARTIST;
        cat.key = "__offline_music__";
        cat.isOffline = true;
        cat.summary = std::to_string(tracks.size()) + " track" + (tracks.size() == 1 ? "" : "s") + " across " + std::to_string(uniqueArtists.size()) + " artist" + (uniqueArtists.size() == 1 ? "" : "s");
        categories.push_back(cat);
    }
    return categories;
}

static std::vector<PlexMediaItem> getOfflineItemsForNavKey(const std::string& navKey) {
    auto allDownloads = g_downloadManager.getDownloadedItems();

    if (navKey == "__offline__") {
        return buildOfflineCategories(allDownloads);
    }
    if (navKey == "__offline_movies__") {
        std::vector<PlexMediaItem> movies;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::MOVIE) movies.push_back(it);
        }
        std::sort(movies.begin(), movies.end(), [](const PlexMediaItem& a, const PlexMediaItem& b) {
            return a.title < b.title;
        });
        return movies;
    }
    if (navKey == "__offline_tv__") {
        std::vector<PlexMediaItem> episodes;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::EPISODE) episodes.push_back(it);
        }
        return buildOfflineShows(episodes);
    }
    if (navKey.rfind("__offline_show:", 0) == 0) {
        std::string showName = navKey.substr(15);
        std::vector<PlexMediaItem> episodes;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::EPISODE) episodes.push_back(it);
        }
        return buildOfflineSeasons(episodes, showName);
    }
    if (navKey.rfind("__offline_season:", 0) == 0) {
        std::string rest = navKey.substr(17);
        size_t slash = rest.find('/');
        std::string showName = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
        std::string seasonName = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
        std::vector<PlexMediaItem> episodes;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::EPISODE) episodes.push_back(it);
        }
        return buildOfflineEpisodes(episodes, showName, seasonName);
    }
    if (navKey == "__offline_music__") {
        std::vector<PlexMediaItem> tracks;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::TRACK) tracks.push_back(it);
        }
        return buildOfflineArtists(tracks);
    }
    if (navKey.rfind("__offline_artist:", 0) == 0) {
        std::string artistName = navKey.substr(17);
        std::vector<PlexMediaItem> tracks;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::TRACK) tracks.push_back(it);
        }
        return buildOfflineAlbums(tracks, artistName);
    }
    if (navKey.rfind("__offline_album:", 0) == 0) {
        std::string rest = navKey.substr(16);
        size_t slash = rest.find('/');
        std::string artistName = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
        std::string albumName = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
        std::vector<PlexMediaItem> tracks;
        for (const auto& it : allDownloads) {
            if (it.type == MediaType::TRACK) tracks.push_back(it);
        }
        return buildOfflineTracks(tracks, artistName, albumName);
    }

    return allDownloads;
}

static void deleteOfflineContainer(const std::string& containerKey) {
    auto allDownloads = g_downloadManager.getDownloadedItems();
    for (const auto& it : allDownloads) {
        bool match = false;
        if (containerKey == "__offline_movies__") {
            match = (it.type == MediaType::MOVIE);
        } else if (containerKey == "__offline_tv__") {
            match = (it.type == MediaType::EPISODE);
        } else if (containerKey.rfind("__offline_show:", 0) == 0) {
            std::string show = containerKey.substr(15);
            std::string itShow = !it.grandparentTitle.empty() ? it.grandparentTitle : (!it.parentTitle.empty() ? it.parentTitle : "Unknown Show");
            match = (it.type == MediaType::EPISODE && itShow == show);
        } else if (containerKey.rfind("__offline_season:", 0) == 0) {
            std::string rest = containerKey.substr(17);
            size_t slash = rest.find('/');
            std::string show = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
            std::string szn = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
            std::string itShow = !it.grandparentTitle.empty() ? it.grandparentTitle : (!it.parentTitle.empty() ? it.parentTitle : "Unknown Show");
            std::string itSzn = !it.parentTitle.empty() ? it.parentTitle : "Season 01";
            match = (it.type == MediaType::EPISODE && itShow == show && itSzn == szn);
        } else if (containerKey == "__offline_music__") {
            match = (it.type == MediaType::TRACK);
        } else if (containerKey.rfind("__offline_artist:", 0) == 0) {
            std::string artist = containerKey.substr(17);
            std::string itArtist = !it.grandparentTitle.empty() ? it.grandparentTitle : "Unknown Artist";
            match = (it.type == MediaType::TRACK && itArtist == artist);
        } else if (containerKey.rfind("__offline_album:", 0) == 0) {
            std::string rest = containerKey.substr(16);
            size_t slash = rest.find('/');
            std::string artist = (slash != std::string::npos) ? rest.substr(0, slash) : rest;
            std::string alb = (slash != std::string::npos) ? rest.substr(slash + 1) : "";
            std::string itArtist = !it.grandparentTitle.empty() ? it.grandparentTitle : "Unknown Artist";
            std::string itAlb = !it.parentTitle.empty() ? it.parentTitle : "Unknown Album";
            match = (it.type == MediaType::TRACK && itArtist == artist && itAlb == alb);
        }

        if (match) {
            g_downloadManager.deleteDownload(it.ratingKey);
        }
    }
}

static void openDownloadsView(UIRenderer& ui) {
    (void)ui;
    if (g_state != AppState::ITEM_LIST) {
        s_downloadsReturnState = g_state;
    }
    g_navStack.clear();
    g_currentNavTitle = "Downloads";
    g_currentNavKey = "__offline__";
    g_items = getOfflineItemsForNavKey(g_currentNavKey);
    for (auto& item : g_items) {
        if (g_resumeMap.count(item.ratingKey)) {
            item.viewOffsetMs = std::max(item.viewOffsetMs, g_resumeMap[item.ratingKey]);
        }
    }
    g_state = AppState::ITEM_LIST;
    g_selectedItemIdx = 0;
    g_scrollOffset = 0;
    int64_t freeBytes = g_downloadManager.getSDFreeSpaceBytes();
    int freeGB = (int)(freeBytes / (1024 * 1024 * 1024));
    auto allDownloads = g_downloadManager.getDownloadedItems();
    g_statusMsg = "Downloads (" + std::to_string(allDownloads.size()) + " items, " + std::to_string(freeGB) + " GB free)";
}

static void downloadSingleItem(const PlexMediaItem& item, const PlexServer& server, const PlexAPI& api) {
    if (g_downloadManager.isDownloaded(item.ratingKey)) {
        g_statusMsg = item.title + " is already downloaded";
        return;
    }
    std::string dlUrl = api.buildTranscodeUrl(server, item, g_config);
    if (!dlUrl.empty()) {
        std::vector<std::pair<PlexMediaItem, std::string>> list;
        list.push_back({item, dlUrl});
        g_downloadManager.queueDownloads(list);
        g_statusMsg = "Downloading: " + item.title;
    } else {
        g_statusMsg = "Failed to build download URL";
    }
}

static void downloadContainer(const PlexMediaItem& containerItem, const PlexServer& server, const PlexAPI& api, UIRenderer& ui) {
    g_statusMsg = "Fetching " + containerItem.title + " items...";
    ui.beginFrame();
    ui.renderTopScreen(g_state, g_hasNowPlaying ? &g_nowPlayingItem : nullptr, g_hasNowPlaying, nullptr, g_statusMsg, g_pVideoPlayer, g_pAudioPlayer, g_config.username, !g_config.authToken.empty(), g_pinCode);
    ui.endFrame();

    std::vector<PlexMediaItem> childItems;
    if (!const_cast<PlexAPI&>(api).getItems(server, containerItem.key, childItems, 0, 100)) {
        g_statusMsg = "Failed to fetch items for " + containerItem.title;
        return;
    }

    std::vector<std::pair<PlexMediaItem, std::string>> queueList;
    for (auto child : childItems) {
        if (!child.ratingKey.empty() && child.key != "__LOAD_MORE__" && !g_downloadManager.isDownloaded(child.ratingKey)) {
            if (containerItem.type == MediaType::SEASON) {
                if (child.parentTitle.empty()) child.parentTitle = containerItem.title;
                if (child.grandparentTitle.empty() && !containerItem.parentTitle.empty()) child.grandparentTitle = containerItem.parentTitle;
            } else if (containerItem.type == MediaType::ALBUM) {
                if (child.parentTitle.empty()) child.parentTitle = containerItem.title;
                if (child.grandparentTitle.empty() && !containerItem.parentTitle.empty()) child.grandparentTitle = containerItem.parentTitle;
            }
            std::string dlUrl = api.buildTranscodeUrl(server, child, g_config);
            if (!dlUrl.empty()) {
                queueList.push_back({child, dlUrl});
            }
        }
    }

    if (queueList.empty()) {
        g_statusMsg = containerItem.title + " is already downloaded!";
        return;
    }

    int queued = g_downloadManager.queueDownloads(queueList);
    std::string unit = (containerItem.type == MediaType::ALBUM) ? "tracks" : "episodes";
    g_statusMsg = "Queued " + std::to_string(queued) + " " + unit + " from " + containerItem.title;
}

static void downloadCurrentList(const PlexServer& server, const PlexAPI& api, const std::string& title) {
    std::vector<std::pair<PlexMediaItem, std::string>> queueList;
    for (auto child : g_items) {
        if (!child.ratingKey.empty() && child.key != "__LOAD_MORE__" && !g_downloadManager.isDownloaded(child.ratingKey)) {
            if (child.parentTitle.empty() && !title.empty()) {
                child.parentTitle = title;
            }
            std::string dlUrl = api.buildTranscodeUrl(server, child, g_config);
            if (!dlUrl.empty()) {
                queueList.push_back({child, dlUrl});
            }
        }
    }

    if (queueList.empty()) {
        g_statusMsg = title + " is already downloaded!";
        return;
    }

    int queued = g_downloadManager.queueDownloads(queueList);
    g_statusMsg = "Queued " + std::to_string(queued) + " items from " + title;
}

static void playMediaItem(const PlexMediaItem& item, AudioPlayer& audioPlayer, VideoPlayer& videoPlayer, const PlexAPI& api, int64_t startOffsetMs = 0) {
    g_nowPlayingItem = item;
    g_nowPlayingItem.viewOffsetMs = startOffsetMs;
    g_hasNowPlaying = true;
    g_controlsExpanded = true;
    g_isScrubbing = false;

    std::string playUrl;
    if (item.isOffline || g_downloadManager.isDownloaded(item.ratingKey)) {
        playUrl = !item.localFilePath.empty() ? item.localFilePath : g_downloadManager.getLocalFilePath(item.ratingKey);
    } else {
        if (!g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
            playUrl = api.buildTranscodeUrl(g_servers[g_selectedServerIdx], item, g_config);
        }
    }

    if (playUrl.empty()) {
        g_statusMsg = "Error: Invalid media URL";
        g_hasNowPlaying = false;
        g_controlsExpanded = false;
        return;
    }

    if (item.type == MediaType::TRACK) {
        videoPlayer.stop();
#ifdef __3DS__
        // Maintain 804MHz speedup during audio playback for stutter-free MP3/AAC decoding & TLS decryption
        osSetSpeedupEnable(true);
        // Disallow hardware sleep for clamshell playback only if headphones connected
        aptSetSleepAllowed(!isHeadphoneConnected());
#endif
        audioPlayer.play(playUrl, (int)(item.durationMs / 1000));
        if (startOffsetMs > 0) {
            audioPlayer.seekTo((int)(startOffsetMs / 1000));
        }
    } else {
        audioPlayer.stop();
#ifdef __3DS__
        // Dynamically engage 804MHz CPU clock & L2 cache on New 3DS exclusively for H.264 video decoding
        osSetSpeedupEnable(true);
#endif
        videoPlayer.start(playUrl, item.durationMs, startOffsetMs);
        if (!item.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
            const_cast<PlexAPI&>(api).reportTimeline(g_servers[g_selectedServerIdx], item, startOffsetMs, "playing");
        }
    }
}

static bool canPlayPrev() {
    return g_playlistIndex > 0;
}

static bool canPlayNext() {
    return g_playlistIndex >= 0 && g_playlistIndex < (int)g_playlistItems.size() - 1;
}

static void playPrevTrack(AudioPlayer& audioPlayer, VideoPlayer& videoPlayer, const PlexAPI& api) {
    if (g_playlistItems.empty() || g_playlistIndex < 0) return;

    int curSec = (g_nowPlayingItem.type == MediaType::TRACK) ? audioPlayer.getCurrentSeconds() : videoPlayer.getCurrentSeconds();
    if (curSec > 3 || g_playlistIndex == 0) {
        if (g_nowPlayingItem.type == MediaType::TRACK) {
            audioPlayer.seekTo(0);
        } else {
            videoPlayer.seekTo(0);
        }
    } else if (g_playlistIndex > 0) {
        g_playlistIndex--;
        playMediaItem(g_playlistItems[g_playlistIndex], audioPlayer, videoPlayer, api);
    }
}

static void playNextTrack(AudioPlayer& audioPlayer, VideoPlayer& videoPlayer, const PlexAPI& api) {
    if (g_playlistItems.empty() || g_playlistIndex < 0) return;
    if (g_playlistIndex < (int)g_playlistItems.size() - 1) {
        g_playlistIndex++;
        playMediaItem(g_playlistItems[g_playlistIndex], audioPlayer, videoPlayer, api);
    }
}

static void stopAllPlayback(AudioPlayer& audioPlayer, VideoPlayer& videoPlayer, const PlexAPI* api = nullptr) {
    if (g_hasNowPlaying && g_nowPlayingItem.type != MediaType::TRACK) {
        int64_t curMs = videoPlayer.getCurrentTimeMs();
        if (curMs > 10000 && (g_nowPlayingItem.durationMs <= 0 || curMs < g_nowPlayingItem.durationMs - 15000)) {
            g_resumeMap[g_nowPlayingItem.ratingKey] = curMs;
            saveResume();
            if (g_nowPlayingItem.isOffline || g_downloadManager.isDownloaded(g_nowPlayingItem.ratingKey)) {
                g_downloadManager.updatePlaybackOffset(g_nowPlayingItem.ratingKey, curMs);
            }
            if (api && !g_nowPlayingItem.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
                const_cast<PlexAPI*>(api)->reportTimeline(g_servers[g_selectedServerIdx], g_nowPlayingItem, curMs, "stopped");
            }
        }
    }
    audioPlayer.stop();
    videoPlayer.stop();
#ifdef __3DS__
    // Return to standard 268MHz clock rate when playback stops
    osSetSpeedupEnable(false);
#endif
    g_hasNowPlaying = false;
    g_controlsExpanded = false;
    g_isScrubbing = false;
}

static bool promptKeyboard(const std::string& hint, std::string& outText, bool isPassword = false, const std::string& initialText = "") {
#ifdef __3DS__
    SwkbdState swkbd;
    char buffer[256] = {0};
    swkbdInit(&swkbd, SWKBD_TYPE_NORMAL, 2, 255);
    swkbdSetValidation(&swkbd, SWKBD_NOTEMPTY_NOTBLANK, 0, 0);
    swkbdSetFeatures(&swkbd, SWKBD_DEFAULT_QWERTY | SWKBD_DARKEN_TOP_SCREEN);
    swkbdSetHintText(&swkbd, hint.c_str());
    if (isPassword) {
        swkbdSetPasswordMode(&swkbd, SWKBD_PASSWORD_HIDE);
    }
    if (!initialText.empty()) {
        swkbdSetInitialText(&swkbd, initialText.c_str());
    }
    SwkbdButton btn = swkbdInputText(&swkbd, buffer, sizeof(buffer));
    if (btn == SWKBD_BUTTON_CONFIRM || btn == SWKBD_BUTTON_RIGHT) {
        outText = buffer;
        return true;
    }
    return false;
#else
    (void)hint; (void)outText; (void)isPassword; (void)initialText;
    return false;
#endif
}

static void loadConfig() {
    FILE* f = fopen("sdmc:/3ds/plex-3ds/config.json", "rb");
    if (!f) return;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    std::string content(size, '\0');
    fread(&content[0], 1, size, f);
    fclose(f);

    cJSON* root = cJSON_Parse(content.c_str());
    if (!root) return;

    cJSON* token = cJSON_GetObjectItem(root, "token");
    cJSON* user = cJSON_GetObjectItem(root, "username");
    cJSON* sUrl = cJSON_GetObjectItem(root, "serverUrl");
    cJSON* sName = cJSON_GetObjectItem(root, "serverName");
    cJSON* subs = cJSON_GetObjectItem(root, "subtitles");

    if (token && token->valuestring) g_config.authToken = token->valuestring;
    if (user && user->valuestring) g_config.username = user->valuestring;
    if (sUrl && sUrl->valuestring) g_config.serverUrl = sUrl->valuestring;
    if (sName && sName->valuestring) g_config.serverName = sName->valuestring;
    if (subs && cJSON_IsBool(subs)) g_config.subtitlesEnabled = cJSON_IsTrue(subs);
    cJSON* scrOff = cJSON_GetObjectItem(root, "screenOffTimeout");
    if (scrOff && cJSON_IsNumber(scrOff)) g_config.screenOffTimeoutSec = scrOff->valueint;

    cJSON* serversArr = cJSON_GetObjectItem(root, "servers");
    if (serversArr && cJSON_IsArray(serversArr)) {
        int sCount = cJSON_GetArraySize(serversArr);
        for (int i = 0; i < sCount; i++) {
            cJSON* sItem = cJSON_GetArrayItem(serversArr, i);
            PlexServer srv;
            cJSON* name = cJSON_GetObjectItem(sItem, "name");
            cJSON* cid = cJSON_GetObjectItem(sItem, "clientIdentifier");
            cJSON* tok = cJSON_GetObjectItem(sItem, "accessToken");
            cJSON* uri = cJSON_GetObjectItem(sItem, "selectedUri");
            cJSON* custom = cJSON_GetObjectItem(sItem, "isCustom");

            if (name && name->valuestring) srv.name = name->valuestring;
            if (cid && cid->valuestring) srv.clientIdentifier = cid->valuestring;
            if (tok && tok->valuestring) srv.accessToken = tok->valuestring;
            if (uri && uri->valuestring) srv.selectedUri = uri->valuestring;
            if (custom && cJSON_IsBool(custom)) srv.isCustom = cJSON_IsTrue(custom);

            if (!srv.selectedUri.empty()) {
                PlexConnection conn;
                conn.uri = srv.selectedUri;
                conn.local = true;
                conn.relay = false;
                srv.connections.push_back(conn);
            }
            if (!srv.selectedUri.empty() || !srv.name.empty()) {
                g_servers.push_back(srv);
            }
        }
    }

    // Fallback: If no servers in array but serverUrl/serverName present, populate g_servers
    if (g_servers.empty() && !g_config.serverUrl.empty()) {
        PlexServer srv;
        srv.name = g_config.serverName.empty() ? "Plex Server" : g_config.serverName;
        srv.selectedUri = g_config.serverUrl;
        srv.accessToken = g_config.authToken;
        PlexConnection conn;
        conn.uri = g_config.serverUrl;
        conn.local = true;
        conn.relay = false;
        srv.connections.push_back(conn);
        g_servers.push_back(srv);
    }

    cJSON_Delete(root);
}

static void saveConfig() {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "clientIdentifier", g_config.clientIdentifier.c_str());
    cJSON_AddStringToObject(root, "token", g_config.authToken.c_str());
    cJSON_AddStringToObject(root, "username", g_config.username.c_str());
    cJSON_AddStringToObject(root, "serverUrl", g_config.serverUrl.c_str());
    cJSON_AddStringToObject(root, "serverName", g_config.serverName.c_str());
    cJSON_AddNumberToObject(root, "videoBitrate", g_config.maxBitrate);
    cJSON_AddStringToObject(root, "videoResolution", g_config.resolution.c_str());
    cJSON_AddBoolToObject(root, "subtitles", g_config.subtitlesEnabled);
    cJSON_AddNumberToObject(root, "screenOffTimeout", g_config.screenOffTimeoutSec);

    cJSON* srvArr = cJSON_CreateArray();
    for (const auto& s : g_servers) {
        cJSON* sObj = cJSON_CreateObject();
        cJSON_AddStringToObject(sObj, "name", s.name.c_str());
        cJSON_AddStringToObject(sObj, "clientIdentifier", s.clientIdentifier.c_str());
        cJSON_AddStringToObject(sObj, "accessToken", s.accessToken.c_str());
        cJSON_AddStringToObject(sObj, "selectedUri", s.selectedUri.c_str());
        cJSON_AddBoolToObject(sObj, "isCustom", s.isCustom);
        cJSON_AddItemToArray(srvArr, sObj);
    }
    cJSON_AddItemToObject(root, "servers", srvArr);

    char* jsonStr = cJSON_Print(root);
    if (jsonStr) {
#ifdef __3DS__
        mkdir("sdmc:/3ds", 0777);
        mkdir("sdmc:/3ds/plex-3ds", 0777);
#endif
        FILE* f = fopen("sdmc:/3ds/plex-3ds/config.json", "wb");
        if (f) {
            fputs(jsonStr, f);
            fclose(f);
        }
        free(jsonStr);
    }
    cJSON_Delete(root);
}

static void actionSyncServers(PlexAPI& api, UIRenderer& ui);

static void actionAddServerByIp(PlexAPI& api, UIRenderer& ui) {
    std::string ip;
    if (!promptKeyboard("Enter Server IP:Port (e.g. 192.168.1.50:32400)", ip, false, "")) {
        return;
    }
    if (ip.empty()) return;

    g_statusMsg = "Testing " + ip + "...";
    ui.beginFrame();
    ui.renderTopScreen(g_state, nullptr, false, nullptr, g_statusMsg, nullptr, nullptr, g_config.username, !g_config.authToken.empty());
    ui.endFrame();

    PlexServer newServer;
    newServer.name = "Custom Server";
    newServer.selectedUri = ip;
    newServer.accessToken = g_config.authToken;
    newServer.isCustom = true;

    bool ok = api.testServer(newServer);
    if (!ok && g_config.authToken.empty()) {
        std::string tokenInput;
        if (promptKeyboard("Enter Plex Token (or Cancel for none)", tokenInput, false, "")) {
            newServer.accessToken = tokenInput;
            ok = api.testServer(newServer);
        }
    }

    if (ok) {
        bool found = false;
        for (size_t i = 0; i < g_servers.size(); i++) {
            if (g_servers[i].selectedUri == newServer.selectedUri) {
                g_servers[i] = newServer;
                g_selectedServerIdx = (int)i;
                found = true;
                break;
            }
        }
        if (!found) {
            g_servers.push_back(newServer);
            g_selectedServerIdx = (int)g_servers.size() - 1;
        }
        g_config.serverUrl = newServer.selectedUri;
        g_config.serverName = newServer.name;
        saveConfig();
        g_statusMsg = "Added " + newServer.name;
        g_state = AppState::SERVER_SELECT;
    } else {
        g_statusMsg = "Failed to connect to " + ip;
    }
}

static void actionSyncServers(PlexAPI& api, UIRenderer& ui) {
    if (g_config.authToken.empty()) {
        g_statusMsg = "Please sign in to sync servers.";
        g_state = AppState::PIN_AUTH;
        return;
    }

    g_statusMsg = "Syncing servers from plex.tv...";
    ui.beginFrame();
    ui.renderTopScreen(g_state, nullptr, false, nullptr, g_statusMsg, nullptr, nullptr, g_config.username, !g_config.authToken.empty());
    ui.endFrame();

    std::vector<PlexServer> cloudServers;
    if (api.getServers(g_config.authToken, cloudServers)) {
        int added = 0;
        for (const auto& cs : cloudServers) {
            bool found = false;
            for (auto& s : g_servers) {
                if ((!cs.clientIdentifier.empty() && cs.clientIdentifier == s.clientIdentifier) ||
                    (!cs.selectedUri.empty() && cs.selectedUri == s.selectedUri) ||
                    (cs.name == s.name)) {
                    s.name = cs.name;
                    s.clientIdentifier = cs.clientIdentifier;
                    s.accessToken = cs.accessToken;
                    s.connections = cs.connections;
                    if (!cs.selectedUri.empty()) s.selectedUri = cs.selectedUri;
                    found = true;
                    break;
                }
            }
            if (!found) {
                g_servers.push_back(cs);
                added++;
            }
        }
        saveConfig();
        g_statusMsg = "Synced! " + std::to_string(cloudServers.size()) + " servers available.";
    } else {
        g_statusMsg = "Could not find servers on plex.tv account.";
    }
}

static void actionSignInEmail(PlexAPI& api, UIRenderer& ui) {
    std::string email;
    if (!promptKeyboard("Enter Plex Email or Username", email, false, "")) return;
    if (email.empty()) return;

    std::string password;
    if (!promptKeyboard("Enter Plex Password", password, true, "")) return;
    if (password.empty()) return;

    g_statusMsg = "Signing in to Plex...";
    ui.beginFrame();
    ui.renderTopScreen(AppState::PIN_AUTH, nullptr, false, nullptr, g_statusMsg, nullptr, nullptr, "", false);
    ui.endFrame();

    std::string token, username, err;
    bool signedIn = api.signIn(email, password, token, username, err);

    // Explicitly wipe plaintext password buffer from memory
    volatile char* pwData = const_cast<char*>(password.data());
    for (size_t i = 0; i < password.size(); i++) pwData[i] = 0;
    password.clear();

    if (signedIn) {
        g_config.authToken = token;
        g_config.username = username;
        saveConfig();
        g_statusMsg = "Signed in as " + username + "! Syncing...";
        actionSyncServers(api, ui);
        g_state = AppState::SERVER_SELECT;
        g_selectedServerIdx = 0;
    } else {
        g_statusMsg = err.empty() ? "Sign-in failed." : err;
    }
}

static void actionLogout(PlexAPI& api) {
    g_config.authToken = "";
    g_config.username = "";
    saveConfig();
    g_pinId.clear();
    g_pinCode.clear();
    api.requestPin(g_pinId, g_pinCode);
    g_lastPollTime = osGetTime();
    g_statusMsg = "Logged out of Plex account.";
}

static std::string getActiveSubtitleName(const PlexMediaItem& item) {
    if (!g_config.subtitlesEnabled) return "";
    if (item.selectedSubtitleIdx >= 0 && item.selectedSubtitleIdx < (int)item.subtitleTracks.size()) {
        return item.subtitleTracks[item.selectedSubtitleIdx].title;
    }
    return "On";
}

static void toggleCaptions(VideoPlayer& videoPlayer, const PlexAPI& api) {
    if (g_nowPlayingItem.type == MediaType::TRACK) return;

    if (!g_config.subtitlesEnabled) {
        g_config.subtitlesEnabled = true;
        if (!g_nowPlayingItem.subtitleTracks.empty()) {
            g_nowPlayingItem.selectedSubtitleIdx = 0;
            g_statusMsg = "CC: " + g_nowPlayingItem.subtitleTracks[0].title;
        } else {
            g_statusMsg = "Captions: ON";
        }
    } else {
        if (!g_nowPlayingItem.subtitleTracks.empty() && g_nowPlayingItem.selectedSubtitleIdx + 1 < (int)g_nowPlayingItem.subtitleTracks.size()) {
            g_nowPlayingItem.selectedSubtitleIdx++;
            g_statusMsg = "CC: " + g_nowPlayingItem.subtitleTracks[g_nowPlayingItem.selectedSubtitleIdx].title;
        } else {
            g_config.subtitlesEnabled = false;
            g_nowPlayingItem.selectedSubtitleIdx = -1;
            g_statusMsg = "Captions: OFF";
        }
    }
    saveConfig();

    if (g_hasNowPlaying && videoPlayer.isPlaying() && !videoPlayer.isLocalFile()) {
        int curSec = videoPlayer.getCurrentSeconds();
        if (!g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
            std::string newUrl = api.buildTranscodeUrl(g_servers[g_selectedServerIdx], g_nowPlayingItem, g_config);
            videoPlayer.start(newUrl, g_nowPlayingItem.durationMs, (int64_t)curSec * 1000);
        }
    }
}

static void toggleItemCaptions(PlexMediaItem& item) {
    if (item.type == MediaType::TRACK) return;

    if (!g_config.subtitlesEnabled) {
        g_config.subtitlesEnabled = true;
        if (!item.subtitleTracks.empty()) {
            item.selectedSubtitleIdx = 0;
            g_statusMsg = "CC: " + item.subtitleTracks[0].title;
        } else {
            g_statusMsg = "Captions: ON";
        }
    } else {
        if (!item.subtitleTracks.empty() && item.selectedSubtitleIdx + 1 < (int)item.subtitleTracks.size()) {
            item.selectedSubtitleIdx++;
            g_statusMsg = "CC: " + item.subtitleTracks[item.selectedSubtitleIdx].title;
        } else {
            g_config.subtitlesEnabled = false;
            item.selectedSubtitleIdx = -1;
            g_statusMsg = "Captions: OFF";
        }
    }
    saveConfig();
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

#ifdef __3DS__
    // Start at standard 268MHz CPU clock to conserve battery; 804MHz is engaged on demand during video
    osSetSpeedupEnable(false);
    gspLcdInit();
    ptmuInit();
    ndspInit();
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    DSP_GetHeadphoneStatus(&s_lastHeadphoneStatus);
#endif
    srand((unsigned int)time(nullptr));

    // 1. Initialize Graphics & UI Subsystem FIRST!
    UIRenderer ui;
    if (!ui.init()) {
#ifdef __3DS__
        ndspExit();
        ptmuExit();
        gspLcdExit();
#endif
        return 1;
    }

    // 2. Initialize Network Subsystem
    Network::init();

    AudioPlayer audioPlayer;
    audioPlayer.init();

    VideoPlayer videoPlayer;
    videoPlayer.init();

    g_pAudioPlayer = &audioPlayer;
    g_pVideoPlayer = &videoPlayer;
#ifdef __3DS__
    aptHook(&g_aptCookie, onAptHook, nullptr);
    aptSetHomeAllowed(false);
#endif

    g_downloadManager.init();

    // Try loading saved configuration
    loadConfig();
    g_downloadManager.setClientIdentifier(g_config.clientIdentifier);
    videoPlayer.setClientIdentifier(g_config.clientIdentifier);
    PlexAPI api(g_config.clientIdentifier);
    loadResume();

    // If username is empty but token is present, try fetching user info
    if (!g_config.authToken.empty() && g_config.username.empty()) {
        std::string uName, uEmail;
        if (api.getUser(g_config.authToken, uName, uEmail)) {
            g_config.username = uName;
            saveConfig();
        }
    }

    ensureOfflineLibrary();

    if (!g_servers.empty()) {
        // Select saved server if matches serverUrl
        for (size_t i = 0; i < g_servers.size(); i++) {
            if (g_servers[i].selectedUri == g_config.serverUrl) {
                g_selectedServerIdx = (int)i;
                break;
            }
        }
        g_state = AppState::LIBRARY_LIST;
        g_statusMsg = "Connecting to " + g_servers[g_selectedServerIdx].name + "...";
    } else {
        g_state = AppState::PIN_AUTH;
        api.requestPin(g_pinId, g_pinCode);
        g_lastPollTime = osGetTime();
        g_statusMsg = "Sign in or link Plex account.";
    }
    int initialFrames = 0;

#ifdef __3DS__
    while (aptMainLoop()) {
        if (g_appExiting.load()) {
            break;
        }

        if (aptCheckHomePressRejected()) {
            aptClearChainloader();
            g_appExiting = true;
            break;
        }

        if (g_needsPostWakeupResume.exchange(false)) {
            ndspSetMasterVol(1.0f);
            s_bottomScreenOff = false;
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP);
            GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
            s_lastUserActivityTime = osGetTime();
            if (g_videoWasPlayingOnSuspend && g_pVideoPlayer) {
                g_pVideoPlayer->resumeFromSuspend();
                g_videoWasPlayingOnSuspend = false;
            }
            if (g_audioWasPlayingOnSuspend && g_pAudioPlayer) {
                g_pAudioPlayer->resumeFromSuspend();
                g_audioWasPlayingOnSuspend = false;
            }
            bool canClamshell = (g_pAudioPlayer && g_pAudioPlayer->isPlaying() && !g_pAudioPlayer->isPaused()) && isHeadphoneConnected();
            aptSetSleepAllowed(!canClamshell);
        }

        hidScanInput();
        uint32_t kDown = hidKeysDown();
        uint32_t kHeld = hidKeysHeld();
        uint32_t kUp = hidKeysUp();
        static touchPosition lastTouch = {0, 0};
        touchPosition touch;
        hidTouchRead(&touch);
        if (touch.px > 0 || touch.py > 0) {
            lastTouch = touch;
        }

        bool isCurrentlyPlayingMedia = g_hasNowPlaying && (g_nowPlayingItem.type == MediaType::TRACK ? (!audioPlayer.isPaused() && audioPlayer.isPlaying()) : (!videoPlayer.isPaused() && videoPlayer.isPlaying()));
        bool isPlayingMusic = (g_hasNowPlaying && g_nowPlayingItem.type == MediaType::TRACK &&
                               audioPlayer.isPlaying() && !audioPlayer.isPaused());

#ifdef __3DS__
        bool headphonePlugged = isHeadphoneConnected();
        static bool s_prevHeadphonePlugged = s_lastHeadphoneStatus;
        if (s_prevHeadphonePlugged && !headphonePlugged) {
            // Headphones unplugged during active playback
            if (isCurrentlyPlayingMedia) {
                if (g_nowPlayingItem.type == MediaType::TRACK) {
                    audioPlayer.pause();
                } else {
                    videoPlayer.pause();
                }
                g_statusMsg = "Paused (headphones disconnected)";
            }
        }
        s_prevHeadphonePlugged = headphonePlugged;

        // Recheck isPlayingMusic in case unplugging paused playback
        isPlayingMusic = (g_hasNowPlaying && g_nowPlayingItem.type == MediaType::TRACK &&
                          audioPlayer.isPlaying() && !audioPlayer.isPaused());

        // Clamshell playback is ONLY allowed if actively playing music AND headphones are plugged in!
        bool canClamshellPlay = isPlayingMusic && headphonePlugged;

        // Allow sleep mode unless actively playing music through headphones
        aptSetSleepAllowed(!canClamshellPlay);

        // Check clamshell state via PTMU
        u8 shellState = 1;
        if (R_SUCCEEDED(PTMU_GetShellState(&shellState))) {
            bool isClosed = (shellState == 0);
            if (isClosed != s_shellClosed) {
                s_shellClosed = isClosed;
                if (s_shellClosed) {
                    // Clamshell closed: power off both displays to eliminate backlight drain and light bleed
                    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTH);
                } else {
                    // Clamshell opened: restore displays
                    s_bottomScreenOff = false;
                    GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_TOP);
                    GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
                    s_lastUserActivityTime = osGetTime();
                    // If audio was paused because shell was closed without headphones, resume!
                    if (g_audioWasPlayingOnSuspend) {
                        audioPlayer.resume();
                        g_audioWasPlayingOnSuspend = false;
                    }
                }
            }
        }

        // If shell is closed and we cannot do clamshell playback (e.g. no headphones, or paused/stopped/video), sleep immediately
        if (s_shellClosed && !canClamshellPlay) {
            if (isPlayingMusic) {
                g_audioWasPlayingOnSuspend = true;
                audioPlayer.pause();
            }
            aptSetSleepAllowed(true);
            APT_SleepIfShellClosed();
            continue;
        }

        if (s_shellClosed && canClamshellPlay) {
            // Clamshell is closed during music playback with headphones connected:
            // Check if headphones were unplugged while lid was closed!
            if (!isHeadphoneConnected()) {
                // Headphones unplugged while lid was closed: pause and put console to sleep!
                audioPlayer.pause();
                g_audioWasPlayingOnSuspend = false; // Keep paused on wake so speakers don't blast
                g_statusMsg = "Paused (headphones disconnected)";
                s_prevHeadphonePlugged = false;
                aptSetSleepAllowed(true);
                APT_SleepIfShellClosed();
                continue;
            }

            // Physical shoulder buttons (L/R) remain functional for track skipping!
            // Pressing START or L+R toggles play/pause with lid closed!
            if ((kDown & KEY_START) || ((kHeld & KEY_L) && (kDown & KEY_R))) {
                if (audioPlayer.isPaused()) {
                    audioPlayer.resume();
                } else {
                    audioPlayer.pause();
                }
            } else if (kDown & KEY_L) {
                playPrevTrack(audioPlayer, videoPlayer, api);
            } else if (kDown & KEY_R) {
                playNextTrack(audioPlayer, videoPlayer, api);
            }

            // Auto-advance to next track when song finishes
            if (!audioPlayer.isPlaying()) {
                if (canPlayNext()) {
                    playNextTrack(audioPlayer, videoPlayer, api);
                } else {
                    g_hasNowPlaying = false;
                    g_controlsExpanded = false;
                    aptSetSleepAllowed(true);
                    APT_SleepIfShellClosed();
                    continue;
                }
            }

            // Throttle main loop to 50ms while closed (virtually 0% CPU, no GPU rendering)
            svcSleepThread(50000000ULL);
            continue;
        }
#endif

        // --- Bottom Screen Auto-Off & Touch-Wake Logic ---
        if (s_bottomScreenOff) {
            bool wakeByTouch = ((kDown & KEY_TOUCH) != 0) || (((kHeld & KEY_TOUCH) != 0) && (touch.px > 0 || touch.py > 0));
            bool wakeByKey = (kDown & ~KEY_TOUCH) != 0;

            if (wakeByTouch || wakeByKey || !isCurrentlyPlayingMedia) {
                s_bottomScreenOff = false;
#ifdef __3DS__
                GSPLCD_PowerOnBacklight(GSPLCD_SCREEN_BOTTOM);
#endif
                s_lastUserActivityTime = osGetTime();

                if (wakeByTouch) {
                    // Swallow touch for this gesture so underlying controls are NOT triggered
                    s_ignoringTouchUntilRelease = true;
                    kDown &= ~KEY_TOUCH;
                    kHeld &= ~KEY_TOUCH;
                    touch.px = 0;
                    touch.py = 0;
                }
            }
        } else {
            // Screen is currently ON
            if (kUp & KEY_TOUCH) {
                s_ignoringTouchUntilRelease = false;
            }

            if (s_ignoringTouchUntilRelease) {
                kDown &= ~KEY_TOUCH;
                kHeld &= ~KEY_TOUCH;
                touch.px = 0;
                touch.py = 0;
            } else {
                bool hasInput = (kDown != 0) || ((kHeld & ~KEY_TOUCH) != 0) || (((kHeld & KEY_TOUCH) != 0) && (touch.px > 0 || touch.py > 0)) || g_isScrubbing;
                if (hasInput) {
                    s_lastUserActivityTime = osGetTime();
                }
            }

            // If media is actively playing, check inactivity timeout
            if (isCurrentlyPlayingMedia && !g_isScrubbing && g_config.screenOffTimeoutSec > 0) {
                uint64_t now = osGetTime();
                if (now >= s_lastUserActivityTime && (now - s_lastUserActivityTime) >= (uint64_t)g_config.screenOffTimeoutSec * 1000) {
                    s_bottomScreenOff = true;
#ifdef __3DS__
                    GSPLCD_PowerOffBacklight(GSPLCD_SCREEN_BOTTOM);
#endif
                }
            }
        }

        // Auto-repeat for menu navigation (D-pad & Circle Pad)
        static int s_upHoldFrames = 0;
        static int s_downHoldFrames = 0;

        bool isUpHeld = (kHeld & (KEY_UP | KEY_CPAD_UP)) != 0;
        bool isDownHeld = (kHeld & (KEY_DOWN | KEY_CPAD_DOWN)) != 0;
        bool isUpInitial = (kDown & (KEY_UP | KEY_CPAD_UP)) != 0;
        bool isDownInitial = (kDown & (KEY_DOWN | KEY_CPAD_DOWN)) != 0;

        bool navUp = false;
        bool navDown = false;

        const int REPEAT_DELAY_FRAMES = 16;     // ~260ms initial delay before fast scroll starts
        const int REPEAT_INTERVAL_FRAMES = 4;   // ~66ms per step when held (15 items/sec)

        if (isUpHeld) {
            s_upHoldFrames++;
            if (isUpInitial || (s_upHoldFrames > REPEAT_DELAY_FRAMES && ((s_upHoldFrames - REPEAT_DELAY_FRAMES) % REPEAT_INTERVAL_FRAMES == 0))) {
                navUp = true;
            }
        } else {
            s_upHoldFrames = 0;
        }

        if (isDownHeld) {
            s_downHoldFrames++;
            if (isDownInitial || (s_downHoldFrames > REPEAT_DELAY_FRAMES && ((s_downHoldFrames - REPEAT_DELAY_FRAMES) % REPEAT_INTERVAL_FRAMES == 0))) {
                navDown = true;
            }
        } else {
            s_downHoldFrames = 0;
        }

        // Exit immediately on START
        if (kDown & KEY_START) {
            break;
        }

        // Update active players
        if (g_hasNowPlaying) {
            if (g_nowPlayingItem.type == MediaType::TRACK) {
                audioPlayer.update();
            } else {
                videoPlayer.update();
            }
        }

        // Monitor background downloads
        static bool lastDlActive = false;
        static int lastDlCompletedCount = 0;
        auto dlProg = g_downloadManager.getProgress();
        if (dlProg.active) {
            g_statusMsg = "DL: " + dlProg.title + " - " + dlProg.statusText;
        }

        if (dlProg.completedCount != lastDlCompletedCount) {
            lastDlCompletedCount = dlProg.completedCount;
            if (g_currentNavTitle == "Downloads" || g_currentNavKey.rfind("__offline", 0) == 0) {
                g_items = getOfflineItemsForNavKey(g_currentNavKey);
                int64_t freeBytes = g_downloadManager.getSDFreeSpaceBytes();
                int freeGB = (int)(freeBytes / (1024 * 1024 * 1024));
                auto allDownloads = g_downloadManager.getDownloadedItems();
                g_statusMsg = "Downloads (" + std::to_string(allDownloads.size()) + " items, " + std::to_string(freeGB) + " GB free)";
            } else {
                for (auto& it : g_items) {
                    if (g_downloadManager.isDownloaded(it.ratingKey)) {
                        it.isOffline = true;
                        if (it.localFilePath.empty()) {
                            it.localFilePath = g_downloadManager.getLocalFilePath(it.ratingKey);
                        }
                    }
                }
            }
        }

        if (lastDlActive && !dlProg.active) {
            if (dlProg.completed) {
                g_statusMsg = "Downloaded: " + dlProg.title;
                if (g_currentNavTitle == "Downloads" || g_currentNavKey.rfind("__offline", 0) == 0) {
                    g_items = getOfflineItemsForNavKey(g_currentNavKey);
                } else {
                    for (auto& it : g_items) {
                        if (it.ratingKey == dlProg.ratingKey) {
                            it.isOffline = true;
                            if (it.localFilePath.empty()) {
                                it.localFilePath = g_downloadManager.getLocalFilePath(it.ratingKey);
                            }
                        }
                    }
                }
            } else if (dlProg.failed) {
                g_statusMsg = dlProg.statusText.empty() ? "Download failed" : dlProg.statusText;
            }
        }
        lastDlActive = dlProg.active;

        // Check if playback ended, and auto-advance to next track/episode if available
        bool isMediaPlaying = (videoPlayer.isPlaying() || videoPlayer.hasFrame() || audioPlayer.isPlaying());
        if (g_hasNowPlaying && !isMediaPlaying) {
            if (g_nowPlayingItem.type != MediaType::TRACK) {
                bool hadPlayed = (videoPlayer.getCurrentTimeMs() > 3000 || videoPlayer.hasFrame());
                if (!hadPlayed) {
                    std::string err = videoPlayer.getStatusMessage();
                    if (err.empty() || err == "Stopped" || err == "Idle") {
                        err = "Video playback failed";
                    }
                    g_statusMsg = err;
                    g_hasNowPlaying = false;
                    g_controlsExpanded = false;
#ifdef __3DS__
                    osSetSpeedupEnable(false);
#endif
                } else {
                    g_nowPlayingItem.viewOffsetMs = 0;
                    g_resumeMap.erase(g_nowPlayingItem.ratingKey);
                    saveResume();
                    if (g_nowPlayingItem.isOffline || g_downloadManager.isDownloaded(g_nowPlayingItem.ratingKey)) {
                        g_downloadManager.updatePlaybackOffset(g_nowPlayingItem.ratingKey, 0);
                    }
                    for (auto& it : g_items) {
                        if (it.ratingKey == g_nowPlayingItem.ratingKey) {
                            it.viewOffsetMs = 0;
                            break;
                        }
                    }
                    if (!g_nowPlayingItem.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
                        api.reportTimeline(g_servers[g_selectedServerIdx], g_nowPlayingItem, g_nowPlayingItem.durationMs, "stopped");
                    }
                    if (canPlayNext()) {
                        playNextTrack(audioPlayer, videoPlayer, api);
                    } else {
                        g_hasNowPlaying = false;
                        g_controlsExpanded = false;
#ifdef __3DS__
                        osSetSpeedupEnable(false);
#endif
                    }
                }
            } else {
                bool hadPlayed = (audioPlayer.getCurrentSeconds() > 1);
                if (!hadPlayed) {
                    g_statusMsg = "Audio playback failed";
                    g_hasNowPlaying = false;
                    g_controlsExpanded = false;
#ifdef __3DS__
                    osSetSpeedupEnable(false);
#endif
                } else if (canPlayNext()) {
                    playNextTrack(audioPlayer, videoPlayer, api);
                } else {
                    g_hasNowPlaying = false;
                    g_controlsExpanded = false;
#ifdef __3DS__
                    osSetSpeedupEnable(false);
#endif
                }
            }
        }

        // Live playback tracking for resume
        static uint64_t s_lastResumeSaveTick = 0;
        if (g_hasNowPlaying && g_nowPlayingItem.type != MediaType::TRACK && videoPlayer.isPlaying()) {
            int64_t curMs = videoPlayer.getCurrentTimeMs();
            if (curMs > 0) {
                g_nowPlayingItem.viewOffsetMs = curMs;
                for (auto& it : g_items) {
                    if (it.ratingKey == g_nowPlayingItem.ratingKey) {
                        it.viewOffsetMs = curMs;
                        break;
                    }
                }
                uint64_t now = osGetTime();
                if (now - s_lastResumeSaveTick > 10000ULL) { // Periodically save every 10 seconds
                    s_lastResumeSaveTick = now;
                    if (curMs > 10000 && (g_nowPlayingItem.durationMs <= 0 || curMs < g_nowPlayingItem.durationMs - 15000)) {
                        g_resumeMap[g_nowPlayingItem.ratingKey] = curMs;
                        saveResume();
                        if (g_nowPlayingItem.isOffline || g_downloadManager.isDownloaded(g_nowPlayingItem.ratingKey)) {
                            g_downloadManager.updatePlaybackOffset(g_nowPlayingItem.ratingKey, curMs);
                        }
                    }
                }
            }
        }

        // Compute playback time & duration
        int currentSec = 0;
        int totalSec = 0;
        bool isPaused = false;
        if (g_hasNowPlaying) {
            if (g_nowPlayingItem.type == MediaType::TRACK) {
                currentSec = audioPlayer.getCurrentSeconds();
                totalSec = audioPlayer.getTotalSeconds();
                isPaused = audioPlayer.isPaused();
            } else {
                currentSec = videoPlayer.getCurrentSeconds();
                totalSec = videoPlayer.getTotalSeconds();
                isPaused = videoPlayer.isPaused();
            }
            if (totalSec == 0 && g_nowPlayingItem.durationMs > 0) {
                totalSec = (int)(g_nowPlayingItem.durationMs / 1000);
            }
        }

        // Render every frame to ensure screen is never black
        ui.beginFrame();
        PlexMediaItem* currentDetailItem = ((g_state == AppState::DETAIL_VIEW || g_state == AppState::ITEM_LIST) && !g_items.empty() && g_selectedItemIdx < (int)g_items.size()) 
                                           ? &g_items[g_selectedItemIdx] : nullptr;

        ui.renderTopScreen(g_state,
                           g_hasNowPlaying ? &g_nowPlayingItem : nullptr,
                           g_hasNowPlaying,
                           currentDetailItem,
                           g_statusMsg,
                           &videoPlayer,
                           &audioPlayer,
                           g_config.username,
                           !g_config.authToken.empty(),
                           g_pinCode);

        int selectedIdx = (g_state == AppState::ITEM_LIST ? g_selectedItemIdx : 
                           g_state == AppState::LIBRARY_LIST ? g_selectedLibraryIdx : g_selectedServerIdx);

        std::string dlBadge = "";
        if (dlProg.active) {
            if (!dlProg.statusText.empty()) {
                dlBadge = "DL: " + dlProg.statusText;
            } else if (dlProg.queueCount > 1) {
                dlBadge = "DL (" + std::to_string(dlProg.queueIndex) + "/" + std::to_string(dlProg.queueCount) + "): " + std::to_string(dlProg.percent) + "%";
            } else {
                dlBadge = "DL: " + std::to_string(dlProg.percent) + "%";
            }
        }

        bool isCurrentItemDownloaded = false;
        bool isDownloadingCurrent = false;
        int dlPercent = dlProg.percent;

        if (g_state == AppState::DETAIL_VIEW && currentDetailItem) {
            isCurrentItemDownloaded = currentDetailItem->isOffline || g_downloadManager.isDownloaded(currentDetailItem->ratingKey);
            isDownloadingCurrent = dlProg.active && (dlProg.ratingKey == currentDetailItem->ratingKey);
        }

        std::string subName = "";
        if (g_controlsExpanded && g_hasNowPlaying) {
            subName = getActiveSubtitleName(g_nowPlayingItem);
        } else if (g_state == AppState::DETAIL_VIEW && currentDetailItem) {
            subName = getActiveSubtitleName(*currentDetailItem);
        }

        if (s_bottomScreenOff) {
            ui.renderBlankBottomScreen();
        } else {
            ui.renderBottomScreen(g_state,
                                  g_pinCode,
                                  g_servers,
                                  g_libraries,
                                  g_items,
                                  selectedIdx,
                                  g_scrollOffset,
                                  g_currentNavTitle,
                                  isPaused,
                                  g_hasNowPlaying,
                                  g_hasNowPlaying ? &g_nowPlayingItem : nullptr,
                                  g_controlsExpanded,
                                  currentSec,
                                  totalSec,
                                  g_isScrubbing,
                                  g_scrubSec,
                                  canPlayPrev(),
                                  canPlayNext(),
                                  isCurrentItemDownloaded,
                                  isDownloadingCurrent,
                                  dlPercent,
                                  dlBadge,
                                  g_config.subtitlesEnabled,
                                  subName,
                                  g_config.username,
                                  !g_config.authToken.empty());
        }
        ui.endFrame();

        // Once the screen has rendered several frames, fetch libraries
        if (initialFrames < 3) {
            initialFrames++;
            if (initialFrames == 3 && g_state == AppState::LIBRARY_LIST) {
                if (!g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
                    g_libraries.clear();
                    if (api.getLibraries(g_servers[g_selectedServerIdx], g_libraries)) {
                        g_statusMsg = "Ready (" + std::to_string(g_libraries.size()) + " sections)";
                    } else {
                        g_statusMsg = "Failed to connect to " + g_servers[g_selectedServerIdx].name;
                        g_state = AppState::SERVER_SELECT;
                        auto dlItems = g_downloadManager.getDownloadedItems();
                        if (!dlItems.empty()) {
                            openDownloadsView(ui);
                            g_statusMsg = "Offline: opened Downloads (" + std::to_string(dlItems.size()) + " items)";
                        }
                    }
                    ensureOfflineLibrary();
                } else {
                    g_state = AppState::SERVER_SELECT;
                    auto dlItems = g_downloadManager.getDownloadedItems();
                    if (!dlItems.empty()) {
                        openDownloadsView(ui);
                        g_statusMsg = "Offline: opened Downloads (" + std::to_string(dlItems.size()) + " items)";
                    }
                }
            }
            continue;
        }

        // Global hotkeys while media is active
        if (g_hasNowPlaying) {
            if (kDown & KEY_L) {
                playPrevTrack(audioPlayer, videoPlayer, api);
                kDown &= ~KEY_L;
            }
            if (kDown & KEY_R) {
                playNextTrack(audioPlayer, videoPlayer, api);
                kDown &= ~KEY_R;
            }
            if (kDown & KEY_X) {
                stopAllPlayback(audioPlayer, videoPlayer, &api);
                kDown &= ~KEY_X;
                continue;
            }
            if (kDown & KEY_Y) {
                g_controlsExpanded = !g_controlsExpanded;
                kDown &= ~KEY_Y;
                continue;
            }
        }

        // --- MODE A: EXPANDED PLAYBACK CONTROLS ---
        if (g_controlsExpanded && g_hasNowPlaying) {
            // Touch Scrubber (drag to seek)
            if (kDown & KEY_TOUCH) {
                if (touch.px >= 10 && touch.px <= 310 && touch.py >= 50 && touch.py <= 95) {
                    g_isScrubbing = true;
                    float f = std::clamp((float)(touch.px - 15) / 290.0f, 0.0f, 1.0f);
                    g_scrubSec = (int)(f * totalSec);
                }
            }
            if ((kHeld & KEY_TOUCH) && g_isScrubbing) {
                float f = std::clamp((float)(touch.px - 15) / 290.0f, 0.0f, 1.0f);
                g_scrubSec = (int)(f * totalSec);
            }
            if (kUp & KEY_TOUCH) {
                if (g_isScrubbing) {
                    g_isScrubbing = false;
                    if (g_nowPlayingItem.type == MediaType::TRACK) {
                        audioPlayer.seekTo(g_scrubSec);
                    } else {
                        videoPlayer.seekTo(g_scrubSec);
                    }
                }
            }

            // Expanded Control Buttons
            if (kDown & KEY_TOUCH) {
                int tx = (touch.px > 0 || touch.py > 0) ? touch.px : lastTouch.px;
                int ty = (touch.py > 0 || touch.py > 0) ? touch.py : lastTouch.py;

                // [|<] Prev: (10..65, 104..154)
                if (tx >= 10 && tx <= 65 && ty >= 104 && ty <= 154) {
                    playPrevTrack(audioPlayer, videoPlayer, api);
                }
                // [<< 15s]: (70..125, 104..154)
                else if (tx >= 70 && tx <= 125 && ty >= 104 && ty <= 154) {
                    if (g_nowPlayingItem.type == MediaType::TRACK) audioPlayer.seek(-15);
                    else videoPlayer.seek(-15);
                }
                // [PAUSE / RESUME]: (130..190, 102..156)
                else if (tx >= 130 && tx <= 190 && ty >= 102 && ty <= 156) {
                    if (g_nowPlayingItem.type == MediaType::TRACK) {
                        if (audioPlayer.isPaused()) audioPlayer.resume(); else audioPlayer.pause();
                    } else {
                        if (videoPlayer.isPaused()) {
                            videoPlayer.resume();
                            if (!g_nowPlayingItem.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
                                api.reportTimeline(g_servers[g_selectedServerIdx], g_nowPlayingItem, videoPlayer.getCurrentTimeMs(), "playing");
                            }
                        } else {
                            videoPlayer.pause();
                            if (!g_nowPlayingItem.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
                                api.reportTimeline(g_servers[g_selectedServerIdx], g_nowPlayingItem, videoPlayer.getCurrentTimeMs(), "paused");
                            }
                        }
                    }
                }
                // [15s >>]: (195..250, 104..154)
                else if (tx >= 195 && tx <= 250 && ty >= 104 && ty <= 154) {
                    if (g_nowPlayingItem.type == MediaType::TRACK) audioPlayer.seek(15);
                    else videoPlayer.seek(15);
                }
                // [>|] Next: (255..310, 104..154)
                else if (tx >= 255 && tx <= 310 && ty >= 104 && ty <= 154) {
                    playNextTrack(audioPlayer, videoPlayer, api);
                }
                if (g_nowPlayingItem.type != MediaType::TRACK) {
                    // [v Browse]: (10..106, 156..200)
                    if (tx >= 10 && tx <= 106 && ty >= 156 && ty <= 200) {
                        g_controlsExpanded = false;
                    }
                    // [CC]: (108..212, 156..200)
                    else if (tx >= 108 && tx <= 212 && ty >= 156 && ty <= 200) {
                        toggleCaptions(videoPlayer, api);
                    }
                    // [Stop]: (214..310, 156..200)
                    else if (tx >= 214 && tx <= 310 && ty >= 156 && ty <= 200) {
                        stopAllPlayback(audioPlayer, videoPlayer, &api);
                    }
                } else {
                    // [v Browse Library]: (15..155, 158..202)
                    if (tx >= 15 && tx <= 155 && ty >= 158 && ty <= 202) {
                        g_controlsExpanded = false;
                    }
                    // [Stop Playback]: (165..305, 158..202)
                    else if (tx >= 165 && tx <= 305 && ty >= 158 && ty <= 202) {
                        stopAllPlayback(audioPlayer, videoPlayer, &api);
                    }
                }
            }

            // Physical buttons on expanded controls
            if (kDown & KEY_A) {
                if (g_nowPlayingItem.type == MediaType::TRACK) {
                    if (audioPlayer.isPaused()) audioPlayer.resume(); else audioPlayer.pause();
                } else {
                    if (videoPlayer.isPaused()) videoPlayer.resume(); else videoPlayer.pause();
                }
            }
            if (kDown & KEY_X) {
                if (g_nowPlayingItem.type != MediaType::TRACK) {
                    toggleCaptions(videoPlayer, api);
                }
            }
            if (kDown & KEY_B) {
                g_controlsExpanded = false; // Collapse to browse library!
            }
            if (kDown & KEY_LEFT) {
                if (g_nowPlayingItem.type == MediaType::TRACK) audioPlayer.seek(-15);
                else videoPlayer.seek(-15);
            }
            if (kDown & KEY_RIGHT) {
                if (g_nowPlayingItem.type == MediaType::TRACK) audioPlayer.seek(15);
                else videoPlayer.seek(15);
            }

            continue;
        }

        // --- MINI-PLAYER BAR TOUCH HANDLING (when docked at y >= 192) ---
        if (g_hasNowPlaying && !g_controlsExpanded && (kDown & KEY_TOUCH) && touch.py >= 192) {
            int tx = touch.px;
            if (tx >= 140 && tx < 182) {
                playPrevTrack(audioPlayer, videoPlayer, api);
            } else if (tx >= 182 && tx < 228) {
                if (g_nowPlayingItem.type == MediaType::TRACK) {
                    if (audioPlayer.isPaused()) audioPlayer.resume(); else audioPlayer.pause();
                } else {
                    if (videoPlayer.isPaused()) videoPlayer.resume(); else videoPlayer.pause();
                }
            } else if (tx >= 228 && tx < 270) {
                playNextTrack(audioPlayer, videoPlayer, api);
            } else {
                // Tapped [^] or title area: Expand controls!
                g_controlsExpanded = true;
            }
            continue;
        }

        // --- STATE MACHINE & LIBRARY NAVIGATION ---
        if (g_state == AppState::PIN_AUTH) {
            bool isLoggedIn = !g_config.authToken.empty();

            if (!isLoggedIn) {
                uint64_t now = osGetTime();
                if (g_pinId.empty() || g_pinCode.empty()) {
                    api.requestPin(g_pinId, g_pinCode);
                    g_lastPollTime = now;
                } else if (now - g_lastPollTime > 2500) { // Poll every 2.5 seconds
                    g_lastPollTime = now;
                    std::string token;
                    if (api.checkPin(g_pinId, token)) {
                        g_config.authToken = token;
                        std::string uName, uEmail;
                        if (api.getUser(token, uName, uEmail)) {
                            g_config.username = uName;
                        }
                        saveConfig();
                        g_statusMsg = "Authenticated! Syncing servers...";
                        actionSyncServers(api, ui);
                        g_state = AppState::SERVER_SELECT;
                        g_selectedServerIdx = 0;
                    }
                }
            }

            if (kDown & KEY_TOUCH) {
                int action = ui.handleTouch(g_state, touch.px, touch.py, (int)g_servers.size(), g_hasNowPlaying, isLoggedIn, !g_servers.empty());
                if (action == TOUCH_AUTH_SYNC) {
                    actionSyncServers(api, ui);
                } else if (action == TOUCH_AUTH_SWITCH_ACCOUNT || action == TOUCH_AUTH_SIGN_IN_EMAIL) {
                    actionSignInEmail(api, ui);
                } else if (action == TOUCH_AUTH_LOGOUT) {
                    actionLogout(api);
                } else if (action == TOUCH_AUTH_BACK) {
                    if (!g_servers.empty()) {
                        g_state = AppState::SERVER_SELECT;
                        g_scrollOffset = 0;
                    }
                } else if (action == TOUCH_AUTH_ADD_LOCAL_IP) {
                    actionAddServerByIp(api, ui);
                } else if (action == TOUCH_AUTH_REFRESH_PIN) {
                    g_pinId.clear();
                    g_pinCode.clear();
                    api.requestPin(g_pinId, g_pinCode);
                    g_lastPollTime = osGetTime();
                    g_statusMsg = "Generated new PIN.";
                } else if (action == TOUCH_AUTH_DOWNLOADS) {
                    openDownloadsView(ui);
                }
            }

            if (kDown & KEY_L) {
                openDownloadsView(ui);
            }

            if (kDown & KEY_B) {
                if (!g_servers.empty()) {
                    g_state = AppState::SERVER_SELECT;
                    g_scrollOffset = 0;
                }
            }
            if ((kDown & KEY_X) && isLoggedIn) {
                actionLogout(api);
            }
            if ((kDown & KEY_Y) && isLoggedIn) {
                actionSyncServers(api, ui);
            }
        } else if (g_state == AppState::SERVER_SELECT) {
            int count = (int)g_servers.size();
            int maxVis = g_hasNowPlaying ? 2 : 3;
            if (count > 0) {
                if (navDown) {
                    if (g_selectedServerIdx < count - 1) {
                        g_selectedServerIdx++;
                        if (g_selectedServerIdx >= g_scrollOffset + maxVis) {
                            g_scrollOffset = g_selectedServerIdx - maxVis + 1;
                        }
                    } else {
                        // Wrap to top
                        g_selectedServerIdx = 0;
                        g_scrollOffset = 0;
                    }
                }
                if (navUp) {
                    if (g_selectedServerIdx > 0) {
                        g_selectedServerIdx--;
                        if (g_selectedServerIdx < g_scrollOffset) {
                            g_scrollOffset = g_selectedServerIdx;
                        }
                    } else {
                        // Wrap to bottom
                        g_selectedServerIdx = count - 1;
                        g_scrollOffset = std::max(0, count - maxVis);
                    }
                }
            }
            if (kDown & KEY_TOUCH) {
                int action = ui.handleTouch(g_state, touch.px, touch.py, (int)g_servers.size(), g_hasNowPlaying, !g_config.authToken.empty(), !g_servers.empty());
                if (action >= 0 && action < 1000) {
                    int idx = g_scrollOffset + action;
                    if (idx < (int)g_servers.size()) {
                        g_selectedServerIdx = idx;
                        kDown |= KEY_A;
                    }
                } else if (action == TOUCH_SERVER_ADD_IP) {
                    actionAddServerByIp(api, ui);
                } else if (action == TOUCH_SERVER_SYNC) {
                    actionSyncServers(api, ui);
                } else if (action == TOUCH_SERVER_REMOVE) {
                    kDown |= KEY_X;
                } else if (action == TOUCH_SERVER_DOWNLOADS) {
                    openDownloadsView(ui);
                } else if (action == TOUCH_SERVER_ACCOUNT) {
                    g_state = AppState::PIN_AUTH;
                    if (g_config.authToken.empty() && (g_pinId.empty() || g_pinCode.empty())) {
                        api.requestPin(g_pinId, g_pinCode);
                        g_lastPollTime = osGetTime();
                    }
                }
            }
            if (kDown & KEY_B) {
                openDownloadsView(ui);
            }
            if (kDown & KEY_SELECT) {
                g_state = AppState::PIN_AUTH;
                if (g_config.authToken.empty() && (g_pinId.empty() || g_pinCode.empty())) {
                    api.requestPin(g_pinId, g_pinCode);
                    g_lastPollTime = osGetTime();
                }
            }
            if (kDown & KEY_Y) {
                actionSyncServers(api, ui);
            }
            if ((kDown & KEY_X) && !g_servers.empty()) {
                std::string remName = g_servers[g_selectedServerIdx].name;
                g_servers.erase(g_servers.begin() + g_selectedServerIdx);
                if (g_selectedServerIdx >= (int)g_servers.size()) {
                    g_selectedServerIdx = std::max(0, (int)g_servers.size() - 1);
                }
                saveConfig();
                g_statusMsg = "Removed " + remName;
            }
            if ((kDown & KEY_A) && !g_servers.empty()) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                auto& srv = g_servers[g_selectedServerIdx];
                g_statusMsg = "Connecting to " + srv.name + "...";
                bool ok = false;
                g_libraries.clear();
                if (!srv.selectedUri.empty() && api.getLibraries(srv, g_libraries)) {
                    ok = true;
                } else if (api.selectBestConnection(srv) && api.getLibraries(srv, g_libraries)) {
                    ok = true;
                }
                ensureOfflineLibrary();
                if (ok) {
                    g_config.serverUrl = srv.selectedUri;
                    g_config.serverName = srv.name;
                    saveConfig();

                    g_state = AppState::LIBRARY_LIST;
                    g_selectedLibraryIdx = 0;
                    g_scrollOffset = 0;
                    g_statusMsg = "Connected to " + srv.name;
                } else {
                    g_statusMsg = "Failed to connect to " + srv.name + " (Press B for Downloads)";
                }
            }
        } else if (g_state == AppState::LIBRARY_LIST) {
            int count = (int)g_libraries.size();
            int maxVis = g_hasNowPlaying ? 4 : 5;
            if (count > 0) {
                if (navDown) {
                    if (g_selectedLibraryIdx < count - 1) {
                        g_selectedLibraryIdx++;
                        if (g_selectedLibraryIdx >= g_scrollOffset + maxVis) {
                            g_scrollOffset = g_selectedLibraryIdx - maxVis + 1;
                        }
                    } else {
                        // Wrap to top
                        g_selectedLibraryIdx = 0;
                        g_scrollOffset = 0;
                    }
                }
                if (navUp) {
                    if (g_selectedLibraryIdx > 0) {
                        g_selectedLibraryIdx--;
                        if (g_selectedLibraryIdx < g_scrollOffset) {
                            g_scrollOffset = g_selectedLibraryIdx;
                        }
                    } else {
                        // Wrap to bottom
                        g_selectedLibraryIdx = count - 1;
                        g_scrollOffset = std::max(0, count - maxVis);
                    }
                }
            }
            if (kDown & KEY_TOUCH) {
                int clicked = ui.handleTouch(g_state, touch.px, touch.py, (int)g_libraries.size(), g_hasNowPlaying);
                if (clicked >= 0) {
                    int idx = g_scrollOffset + clicked;
                    if (idx < (int)g_libraries.size()) {
                        g_selectedLibraryIdx = idx;
                        kDown |= KEY_A;
                    }
                }
            }
            if ((kDown & KEY_A) && !g_libraries.empty()) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                if (g_libraries[g_selectedLibraryIdx].key == "__offline__") {
                    openDownloadsView(ui);
                } else {
                    g_statusMsg = "Loading " + g_libraries[g_selectedLibraryIdx].title + "...";
                    g_items.clear();
                    g_navStack.clear();
                    g_currentNavTitle = g_libraries[g_selectedLibraryIdx].title;
                    g_currentNavKey = g_libraries[g_selectedLibraryIdx].key;
                    if (api.getItems(g_servers[g_selectedServerIdx], g_libraries[g_selectedLibraryIdx].key, g_items, 0, 100)) {
                        for (auto& item : g_items) {
                            if (g_downloadManager.isDownloaded(item.ratingKey)) {
                                item.isOffline = true;
                                if (item.localFilePath.empty()) {
                                    item.localFilePath = g_downloadManager.getLocalFilePath(item.ratingKey);
                                }
                            }
                            if (g_resumeMap.count(item.ratingKey)) {
                                item.viewOffsetMs = std::max(item.viewOffsetMs, g_resumeMap[item.ratingKey]);
                            } else if (item.viewOffsetMs > 0) {
                                g_resumeMap[item.ratingKey] = item.viewOffsetMs;
                            }
                        }
                        if (g_items.size() == 100) {
                            PlexMediaItem nextMore;
                            nextMore.key = "__LOAD_MORE__";
                            nextMore.title = "--> [Load Next 100 Items...]";
                            nextMore.type = MediaType::UNKNOWN;
                            g_items.push_back(nextMore);
                        }
                        g_state = AppState::ITEM_LIST;
                        g_selectedItemIdx = 0;
                        g_scrollOffset = 0;
                        g_statusMsg = "Ready (" + std::to_string(g_items.size()) + " items)";
                    } else {
                        g_statusMsg = "Failed to load " + g_libraries[g_selectedLibraryIdx].title;
                    }
                }
            }
            if (kDown & KEY_L) {
                openDownloadsView(ui);
            }
            if (kDown & KEY_B) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                g_state = AppState::SERVER_SELECT;
                g_scrollOffset = 0;
            }
        } else if (g_state == AppState::ITEM_LIST) {
            int maxVis = g_hasNowPlaying ? 4 : 5;
            int count = (int)g_items.size();
            if (count > 0) {
                if (navDown) {
                    if (g_selectedItemIdx < count - 1) {
                        g_selectedItemIdx++;
                        if (g_selectedItemIdx >= g_scrollOffset + maxVis) {
                            g_scrollOffset = g_selectedItemIdx - maxVis + 1;
                        }
                    } else {
                        // Wrap to top
                        g_selectedItemIdx = 0;
                        g_scrollOffset = 0;
                    }
                }
                if (navUp) {
                    if (g_selectedItemIdx > 0) {
                        g_selectedItemIdx--;
                        if (g_selectedItemIdx < g_scrollOffset) {
                            g_scrollOffset = g_selectedItemIdx;
                        }
                    } else {
                        // Wrap to bottom
                        g_selectedItemIdx = count - 1;
                        g_scrollOffset = std::max(0, count - maxVis);
                    }
                }
            }
            if (kDown & KEY_TOUCH) {
                int clicked = ui.handleTouch(g_state, touch.px, touch.py, (int)g_items.size(), g_hasNowPlaying, !g_config.authToken.empty(), !g_servers.empty(), &g_items, g_scrollOffset, g_currentNavTitle);
                if (clicked == TOUCH_ITEM_BACK) {
                    kDown |= KEY_B;
                } else if (clicked == TOUCH_ITEM_DOWNLOAD_ALL) {
                    if (!g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
                        downloadCurrentList(g_servers[g_selectedServerIdx], api, g_currentNavTitle);
                    }
                } else if (clicked >= TOUCH_ITEM_CONTAINER_DL_BASE) {
                    int itemIdx = g_scrollOffset + (clicked - TOUCH_ITEM_CONTAINER_DL_BASE);
                    if (itemIdx >= 0 && itemIdx < (int)g_items.size() && !g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
                        downloadContainer(g_items[itemIdx], g_servers[g_selectedServerIdx], api, ui);
                    }
                } else if (clicked >= 0) {
                    int itemIdx = g_scrollOffset + clicked;
                    if (itemIdx < (int)g_items.size()) {
                        g_selectedItemIdx = itemIdx;
                        kDown |= KEY_A;
                    }
                }
            }
            if ((kDown & KEY_A) && !g_items.empty()) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                auto& item = g_items[g_selectedItemIdx];
                if (item.key == "__LOAD_MORE__") {
                    g_items.pop_back(); // Remove dummy button
                    int curCount = (int)g_items.size();
                    g_statusMsg = "Loading next 100 items...";
                    ui.beginFrame();
                    ui.renderTopScreen(g_state, g_hasNowPlaying ? &g_nowPlayingItem : nullptr, g_hasNowPlaying, nullptr, g_statusMsg, &videoPlayer, &audioPlayer, g_config.username, !g_config.authToken.empty(), g_pinCode);
                    ui.endFrame();

                    std::vector<PlexMediaItem> moreItems;
                    std::string targetKey = g_currentNavKey.empty() ? g_libraries[g_selectedLibraryIdx].key : g_currentNavKey;
                    if (api.getItems(g_servers[g_selectedServerIdx], targetKey, moreItems, curCount, 100)) {
                        for (auto& it : moreItems) {
                            if (g_downloadManager.isDownloaded(it.ratingKey)) {
                                it.isOffline = true;
                                if (it.localFilePath.empty()) {
                                    it.localFilePath = g_downloadManager.getLocalFilePath(it.ratingKey);
                                }
                            }
                            if (g_resumeMap.count(it.ratingKey)) {
                                it.viewOffsetMs = std::max(it.viewOffsetMs, g_resumeMap[it.ratingKey]);
                            } else if (it.viewOffsetMs > 0) {
                                g_resumeMap[it.ratingKey] = it.viewOffsetMs;
                            }
                            g_items.push_back(it);
                        }
                        if (moreItems.size() == 100) {
                            PlexMediaItem nextMore;
                            nextMore.key = "__LOAD_MORE__";
                            nextMore.title = "--> [Load Next 100 Items...]";
                            nextMore.type = MediaType::UNKNOWN;
                            g_items.push_back(nextMore);
                        }
                        g_statusMsg = "Loaded " + std::to_string(g_items.size()) + " items";
                    } else {
                        g_statusMsg = "Failed to load more items";
                    }
                } else if (isMediaContainer(item.type)) {
                    if (item.key.rfind("__offline", 0) == 0) {
                        NavHistory hist = { g_items, g_currentNavTitle, g_currentNavKey, g_selectedItemIdx, g_scrollOffset };
                        g_navStack.push_back(hist);
                        g_currentNavTitle = item.title;
                        g_currentNavKey = item.key;
                        g_items = getOfflineItemsForNavKey(g_currentNavKey);
                        for (auto& it : g_items) {
                            if (g_resumeMap.count(it.ratingKey)) {
                                it.viewOffsetMs = std::max(it.viewOffsetMs, g_resumeMap[it.ratingKey]);
                            }
                        }
                        g_selectedItemIdx = 0;
                        g_scrollOffset = 0;
                        g_statusMsg = "Ready (" + std::to_string(g_items.size()) + " items)";
                    } else {
                        g_statusMsg = "Loading " + item.title + "...";
                        NavHistory hist = { g_items, g_currentNavTitle, g_currentNavKey, g_selectedItemIdx, g_scrollOffset };
                        g_navStack.push_back(hist);
                        g_currentNavTitle = item.title;
                        g_currentNavKey = item.key;

                        std::vector<PlexMediaItem> newItems;
                        if (api.getItems(g_servers[g_selectedServerIdx], item.key, newItems, 0, 100)) {
                            for (auto& it : newItems) {
                                if (g_downloadManager.isDownloaded(it.ratingKey)) {
                                    it.isOffline = true;
                                    if (it.localFilePath.empty()) {
                                        it.localFilePath = g_downloadManager.getLocalFilePath(it.ratingKey);
                                    }
                                }
                                if (g_resumeMap.count(it.ratingKey)) {
                                    it.viewOffsetMs = std::max(it.viewOffsetMs, g_resumeMap[it.ratingKey]);
                                } else if (it.viewOffsetMs > 0) {
                                    g_resumeMap[it.ratingKey] = it.viewOffsetMs;
                                }
                            }
                            if (newItems.size() == 100) {
                                PlexMediaItem nextMore;
                                nextMore.key = "__LOAD_MORE__";
                                nextMore.title = "--> [Load Next 100 Items...]";
                                nextMore.type = MediaType::UNKNOWN;
                                newItems.push_back(nextMore);
                            }
                            g_items = std::move(newItems);
                            g_selectedItemIdx = 0;
                            g_scrollOffset = 0;
                            g_statusMsg = "Ready (" + std::to_string(g_items.size()) + " items)";
                        } else {
                            g_navStack.pop_back();
                            g_statusMsg = "Failed to load " + item.title;
                        }
                    }
                } else {
                    g_state = AppState::DETAIL_VIEW;
                }
            }
            if ((kDown & KEY_X) && !g_items.empty() && g_selectedItemIdx < (int)g_items.size()) {
                auto& it = g_items[g_selectedItemIdx];
                if (it.isOffline || g_downloadManager.isDownloaded(it.ratingKey) || it.key.rfind("__offline", 0) == 0) {
                    if (isMediaContainer(it.type) && it.key.rfind("__offline", 0) == 0) {
                        deleteOfflineContainer(it.key);
                        g_statusMsg = "Deleted " + it.title;
                    } else {
                        g_downloadManager.deleteDownload(it.ratingKey);
                        g_statusMsg = "Deleted " + it.title;
                    }
                    if ((!g_libraries.empty() && g_selectedLibraryIdx >= 0 && g_selectedLibraryIdx < (int)g_libraries.size() && g_libraries[g_selectedLibraryIdx].key == "__offline__") || g_currentNavTitle == "Downloads" || g_currentNavKey.rfind("__offline", 0) == 0) {
                        g_items = getOfflineItemsForNavKey(g_currentNavKey);
                        if (g_selectedItemIdx >= (int)g_items.size()) {
                            g_selectedItemIdx = std::max(0, (int)g_items.size() - 1);
                        }
                        if (g_scrollOffset > g_selectedItemIdx) {
                            g_scrollOffset = std::max(0, g_selectedItemIdx);
                        }
                    } else {
                        it.isOffline = false;
                        it.localFilePath = "";
                    }
                }
            }
            if ((kDown & KEY_Y) && !g_items.empty() && g_selectedItemIdx >= 0 && g_selectedItemIdx < (int)g_items.size() && g_currentNavTitle != "Downloads" && g_currentNavKey.rfind("__offline", 0) != 0) {
                if (!g_servers.empty() && g_selectedServerIdx >= 0 && g_selectedServerIdx < (int)g_servers.size()) {
                    auto& it = g_items[g_selectedItemIdx];
                    if (it.type == MediaType::ALBUM || it.type == MediaType::SEASON) {
                        downloadContainer(it, g_servers[g_selectedServerIdx], api, ui);
                    } else if (!g_items.empty() && (g_items[0].type == MediaType::TRACK || g_items[0].type == MediaType::EPISODE)) {
                        downloadCurrentList(g_servers[g_selectedServerIdx], api, g_currentNavTitle);
                    } else if (!isMediaContainer(it.type) && it.key != "__LOAD_MORE__") {
                        downloadSingleItem(it, g_servers[g_selectedServerIdx], api);
                    }
                }
            }
            if (kDown & KEY_B) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                if (!g_navStack.empty()) {
                    auto prev = g_navStack.back();
                    g_navStack.pop_back();
                    g_items = std::move(prev.items);
                    g_currentNavTitle = prev.title;
                    g_currentNavKey = prev.navKey;
                    g_selectedItemIdx = prev.selectedIdx;
                    g_scrollOffset = prev.scrollOffset;
                    g_statusMsg = "Back to " + g_currentNavTitle;
                } else {
                    if (g_currentNavTitle == "Downloads" || g_currentNavKey.rfind("__offline", 0) == 0) {
                        g_state = s_downloadsReturnState;
                    } else {
                        g_state = AppState::LIBRARY_LIST;
                    }
                    g_scrollOffset = 0;
                }
            }
        } else if (g_state == AppState::DETAIL_VIEW) {
            if (g_selectedItemIdx < 0 || g_selectedItemIdx >= (int)g_items.size()) {
                g_state = AppState::ITEM_LIST;
                continue;
            }

            auto& curItem = g_items[g_selectedItemIdx];
            bool isItemDownloaded = curItem.isOffline || g_downloadManager.isDownloaded(curItem.ratingKey);
            auto curProg = g_downloadManager.getProgress();
            bool isCurDownloading = curProg.active && (curProg.ratingKey == curItem.ratingKey);

            bool hasResume = (curItem.type != MediaType::TRACK) &&
                             (curItem.viewOffsetMs > 10000) &&
                             (curItem.durationMs <= 0 || curItem.viewOffsetMs < curItem.durationMs - 15000);

            bool actionResume = false;
            bool actionRestart = false;
            bool actionDlOrDel = false;
            bool actionBack = false;
            bool actionCancelDl = false;

            if (kDown & KEY_TOUCH) {
                int tx = (touch.px > 0 || touch.py > 0) ? touch.px : lastTouch.px;
                int ty = (touch.py > 0 || touch.py > 0) ? touch.py : lastTouch.py;

                if (isCurDownloading) {
                    // Cancel DL: (15..155, 150..184)
                    if (tx >= 10 && tx <= 160 && ty >= 145 && ty <= 190) {
                        actionCancelDl = true;
                    }
                    // Back: (165..305, 150..184)
                    else if (tx >= 160 && tx <= 310 && ty >= 145 && ty <= 190) {
                        actionBack = true;
                    }
                } else {
                    if (hasResume) {
                        // Row 1: Captions (15..150, 100..138), DL/Del (152..235, 100..138), Back (236..310, 100..138)
                        if (tx >= 10 && tx <= 152 && ty >= 100 && ty <= 138) {
                            toggleItemCaptions(curItem);
                        } else if (tx >= 153 && tx <= 235 && ty >= 100 && ty <= 138) {
                            actionDlOrDel = true;
                        } else if (tx >= 236 && tx <= 310 && ty >= 100 && ty <= 138) {
                            actionBack = true;
                        }
                        // Row 2: Resume (15..162, 140..186), Restart (163..310, 140..186)
                        else if (tx >= 10 && tx <= 162 && ty >= 140 && ty <= 186) {
                            actionResume = true;
                        } else if (tx >= 163 && tx <= 310 && ty >= 140 && ty <= 186) {
                            actionRestart = true;
                        }
                    } else {
                        // Captions toggle button: (15..195, 104..140)
                        if (curItem.type != MediaType::TRACK && tx >= 12 && tx <= 195 && ty >= 104 && ty <= 140) {
                            toggleItemCaptions(curItem);
                        }
                        // Play: (15..145, 144..180)
                        else if (tx >= 10 && tx <= 147 && ty >= 140 && ty <= 186) {
                            actionRestart = true;
                        }
                        // Download / Delete: (150..228, 144..180)
                        else if (tx >= 148 && tx <= 230 && ty >= 140 && ty <= 186) {
                            actionDlOrDel = true;
                        }
                        // Back: (233..305, 144..180)
                        else if (tx >= 231 && tx <= 310 && ty >= 140 && ty <= 186) {
                            actionBack = true;
                        }
                    }
                }
            }

            if (kDown & KEY_SELECT) {
                if (!isCurDownloading && curItem.type != MediaType::TRACK) {
                    toggleItemCaptions(curItem);
                }
            }

            if (kDown & KEY_A) {
                if (isCurDownloading) {
                    actionCancelDl = true;
                } else if (hasResume) {
                    actionResume = true;
                } else {
                    actionRestart = true;
                }
            }
            if (kDown & KEY_Y) {
                if (!isCurDownloading) {
                    actionRestart = true;
                }
            }
            if (kDown & KEY_X) {
                if (isCurDownloading) {
                    actionCancelDl = true;
                } else {
                    actionDlOrDel = true;
                }
            }
            if (kDown & KEY_B) {
                actionBack = true;
            }

            if (actionCancelDl) {
                g_downloadManager.cancelDownload();
                g_statusMsg = "Download cancelled";
            } else if (actionResume || actionRestart) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                g_playlistItems = g_items;
                g_playlistIndex = g_selectedItemIdx;
                if (isItemDownloaded) {
                    curItem.isOffline = true;
                    if (curItem.localFilePath.empty()) {
                        curItem.localFilePath = g_downloadManager.getLocalFilePath(curItem.ratingKey);
                    }
                }

                int64_t startOffset = 0;
                if (actionResume && hasResume) {
                    startOffset = curItem.viewOffsetMs;
                } else {
                    curItem.viewOffsetMs = 0;
                    g_resumeMap[curItem.ratingKey] = 0;
                    saveResume();
                    if (isItemDownloaded) {
                        g_downloadManager.updatePlaybackOffset(curItem.ratingKey, 0);
                    }
                }

                playMediaItem(curItem, audioPlayer, videoPlayer, api, startOffset);
                g_state = AppState::ITEM_LIST;
            } else if (actionDlOrDel) {
                if (isItemDownloaded) {
                    g_downloadManager.deleteDownload(curItem.ratingKey);
                    g_statusMsg = "Deleted " + curItem.title;
                    if ((!g_libraries.empty() && g_selectedLibraryIdx >= 0 && g_selectedLibraryIdx < (int)g_libraries.size() && g_libraries[g_selectedLibraryIdx].key == "__offline__") || g_currentNavTitle == "Downloads" || g_currentNavKey.rfind("__offline", 0) == 0) {
                        g_items = getOfflineItemsForNavKey(g_currentNavKey);
                        if (g_selectedItemIdx >= (int)g_items.size()) {
                            g_selectedItemIdx = std::max(0, (int)g_items.size() - 1);
                        }
                        if (g_scrollOffset > g_selectedItemIdx) {
                            g_scrollOffset = std::max(0, g_selectedItemIdx);
                        }
                        g_state = AppState::ITEM_LIST;
                    } else {
                        curItem.isOffline = false;
                        curItem.localFilePath = "";
                    }
                } else {
                    if (g_downloadManager.isDownloading()) {
                        g_statusMsg = "Already downloading an item!";
                    } else if (g_servers.empty() || g_selectedServerIdx < 0 || g_selectedServerIdx >= (int)g_servers.size()) {
                        g_statusMsg = "No active server to download from";
                    } else {
                        std::string dlUrl = api.buildTranscodeUrl(g_servers[g_selectedServerIdx], curItem, g_config);
                        if (!dlUrl.empty()) {
                            g_downloadManager.startDownload(curItem, dlUrl);
                            g_statusMsg = "Downloading: " + curItem.title;
                        } else {
                            g_statusMsg = "Failed to build download URL";
                        }
                    }
                }
            } else if (actionBack) {
                s_upHoldFrames = 0;
                s_downHoldFrames = 0;
                g_state = AppState::ITEM_LIST;
            }
        } else if (g_state == AppState::PLAYING_AUDIO || g_state == AppState::PLAYING_VIDEO) {
            g_controlsExpanded = true;
            g_state = AppState::ITEM_LIST;
        }
    }
#endif

    // Clean exit
#ifdef __3DS__
    aptUnhook(&g_aptCookie);
    aptSetSleepAllowed(true);
    if (s_bottomScreenOff) {
        s_bottomScreenOff = false;
    }
    GSPLCD_PowerOnAllBacklights();
    gspLcdExit();
    ptmuExit();
#endif
    if (g_hasNowPlaying && g_nowPlayingItem.type != MediaType::TRACK) {
        int64_t curMs = videoPlayer.getCurrentTimeMs();
        if (curMs > 10000 && (g_nowPlayingItem.durationMs <= 0 || curMs < g_nowPlayingItem.durationMs - 15000)) {
            g_resumeMap[g_nowPlayingItem.ratingKey] = curMs;
            if (g_nowPlayingItem.isOffline || g_downloadManager.isDownloaded(g_nowPlayingItem.ratingKey)) {
                g_downloadManager.updatePlaybackOffset(g_nowPlayingItem.ratingKey, curMs);
            }
            if (!g_nowPlayingItem.isOffline && !g_servers.empty() && g_selectedServerIdx >= 0) {
                api.reportTimeline(g_servers[g_selectedServerIdx], g_nowPlayingItem, curMs, "stopped");
            }
        }
    }
    saveResume();

    g_downloadManager.exit();
    videoPlayer.exit();
    audioPlayer.exit();
    ui.exit();
#ifdef __3DS__
    ndspExit();
#endif
    Network::exit();
    return 0;
}
