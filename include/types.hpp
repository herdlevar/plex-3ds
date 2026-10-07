#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <atomic>

extern std::atomic<bool> g_appExiting;
extern std::atomic<bool> g_isSuspended;

enum class AppState {
    PIN_AUTH,         // Showing 4-letter PIN code on screen
    SERVER_SELECT,    // Picking which Plex Media Server to connect to
    LIBRARY_LIST,     // Viewing Sections (Movies, TV Shows, Music)
    ITEM_LIST,        // Grid/List of items in selected section
    DETAIL_VIEW,      // Viewing synopsis, metadata, and play button
    PLAYING_VIDEO,    // Video player active (fullscreen top screen)
    PLAYING_AUDIO,    // Music player active
    SETTINGS,         // Resolution/bitrate adjustments
    ERROR_STATE       // Error message display
};

enum class MediaType {
    UNKNOWN,
    MOVIE,
    SHOW,
    SEASON,
    EPISODE,
    ARTIST,
    ALBUM,
    TRACK
};

inline bool isMediaContainer(MediaType t) {
    return t == MediaType::SHOW || t == MediaType::SEASON ||
           t == MediaType::ARTIST || t == MediaType::ALBUM;
}

inline bool isPlayableMedia(MediaType t) {
    return t == MediaType::MOVIE || t == MediaType::EPISODE || t == MediaType::TRACK;
}

struct PlexConnection {
    std::string uri;
    bool local;
    bool relay;
};

struct PlexServer {
    std::string name;
    std::string clientIdentifier;
    std::string accessToken;
    std::string selectedUri;
    std::vector<PlexConnection> connections;
    bool isCustom = false;
};

struct PlexLibrary {
    std::string key;
    std::string title;
    std::string type; // "movie", "show", "artist"
};

struct PlexSubtitleTrack {
    int id = 0;
    std::string language;
    std::string languageCode;
    std::string title;
    std::string format;
    bool isSelected = false;
    bool isForced = false;
};

struct PlexMediaItem {
    std::string ratingKey;
    std::string key;
    std::string title;
    std::string parentTitle;      // e.g. Artist name, Album name, Show name
    std::string grandparentTitle; // e.g. Show name for episodes
    std::string summary;
    std::string thumbUrl;
    std::string artUrl;
    std::string duration;
    int64_t durationMs = 0;
    int64_t viewOffsetMs = 0; // Resume position in milliseconds
    int year = 0;
    int index = 0; // Track or episode number
    MediaType type = MediaType::UNKNOWN;
    
    // Video / Audio parts
    std::string partKey;
    std::string container;
    std::string transcodeUrl;

    // Offline download support
    bool isOffline = false;
    std::string localFilePath;
    int64_t localFileSize = 0;

    // Subtitles
    std::vector<PlexSubtitleTrack> subtitleTracks;
    int selectedSubtitleIdx = -1; // -1 = default/off, >= 0 = specific track
};

struct AppConfig {
    std::string clientIdentifier = "plex-3ds-fc4e70c5254a";
    std::string authToken = "";
    std::string username = "";
    std::string serverUrl = "";
    std::string serverName = "";
    int maxBitrate = 1000; // kbps
    std::string resolution = "400x240";
    bool hardwareDecode = true;
    bool subtitlesEnabled = false;
    int screenOffTimeoutSec = 10; // Seconds of inactivity before bottom screen turns off (0 = disabled)
};
